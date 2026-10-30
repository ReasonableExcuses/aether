#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include "aether/typed_array.h"
#include "aether/intl.h"
#include "aether/wasm.h"
#include "aether/advanced_intrinsics.h"
#include "aether/bounded_regex.h"
#include <random>
#include <cmath>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace aether {

static thread_local MicrotaskQueue* s_current_microtasks_{nullptr};
static thread_local Agent* s_current_agent_{nullptr};

static void SetRegexException(const RegexError& error) {
    const bool resource = error.ErrorKind() == RegexErrorKind::StepBudgetExceeded ||
                          error.ErrorKind() == RegexErrorKind::ResourceLimitExceeded;
    if (resource) {
        RaiseResourceError(error.what());
        return;
    }
    if (Interpreter::Current()) {
        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("SyntaxError", error.what())));
    }
}

static void SetRangeException(std::string_view message) {
    if (Interpreter::Current()) {
        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("RangeError", message)));
    }
}

static void SetTypeException(std::string_view message) {
    if (Interpreter::Current()) {
        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("TypeError", message)));
    }
}

static bool StrictEquals(TaggedValue a, TaggedValue b) {
    if (a.IsNumber() && b.IsNumber()) {
        double da = a.AsNumber();
        double db = b.AsNumber();
        if (std::isnan(da) || std::isnan(db)) return false;
        return da == db;
    }
    if (a.IsHeapObject() && b.IsHeapObject()) {
        HeapObject* ha = a.AsHeapObject();
        HeapObject* hb = b.AsHeapObject();
        if (ha->IsString() && hb->IsString()) {
            return static_cast<JSString*>(ha)->Value() == static_cast<JSString*>(hb)->Value();
        }
        return ha == hb;
    }
    return a == b;
}

static void AppendRegexReplacement(std::string& out, std::string_view replacement,
                                   std::string_view input, const RegexMatch& match) {
    for (size_t i = 0; i < replacement.size(); ++i) {
        if (replacement[i] != '$' || i + 1 >= replacement.size()) {
            out.push_back(replacement[i]);
            continue;
        }
        const char next = replacement[i + 1];
        if (next == '$') {
            out.push_back('$');
            ++i;
        } else if (next == '&') {
            const auto range = match.Full();
            out.append(input.substr(range.begin, range.end - range.begin));
            ++i;
        } else if (next >= '0' && next <= '9') {
            size_t group = static_cast<size_t>(next - '0');
            size_t consumed = 1;
            if (i + 2 < replacement.size() && replacement[i + 2] >= '0' && replacement[i + 2] <= '9') {
                const size_t candidate = group * 10 + static_cast<size_t>(replacement[i + 2] - '0');
                if (candidate < match.captures.size()) {
                    group = candidate;
                    consumed = 2;
                }
            }
            if (group < match.captures.size() && match.captures[group].has_value()) {
                const auto range = *match.captures[group];
                out.append(input.substr(range.begin, range.end - range.begin));
                i += consumed;
            } else {
                out.push_back('$');
            }
        } else {
            out.push_back('$');
        }
    }
}

Agent* Agent::Current() noexcept { return s_current_agent_; }
void Agent::SetCurrent(Agent* agent) noexcept { s_current_agent_ = agent; }

MicrotaskQueue& CurrentMicrotaskQueue() {
    static MicrotaskQueue s_fallback_queue;
    if (s_current_microtasks_) {
        return *s_current_microtasks_;
    }
    return s_fallback_queue;
}

void SetCurrentMicrotaskQueue(MicrotaskQueue* queue) {
    s_current_microtasks_ = queue;
}

TaggedValue CallFunction(TaggedValue callee, TaggedValue this_val, const std::vector<TaggedValue>& args, TaggedValue new_target) {
    if (!callee.IsHeapObject()) return TaggedValue::Undefined();
    HeapObject* h_obj = callee.AsHeapObject();
    if (h_obj->IsProxy()) {
        return ProxyApply(static_cast<JSProxy*>(h_obj), this_val, args);
    }
    if (h_obj->IsNativeFunction()) {
        auto* native_fn = static_cast<JSNativeFunction*>(h_obj);
        TaggedValue prev_target = Interpreter::Current() ? Interpreter::Current()->GetNewTarget() : TaggedValue::Undefined();
        if (Interpreter::Current()) {
            Interpreter::Current()->SetNewTarget(new_target);
        }
        TaggedValue res = native_fn->Invoke(this_val, args);
        if (Interpreter::Current()) {
            Interpreter::Current()->SetNewTarget(prev_target);
        }
        return res;
    }
    if (h_obj->IsUserFunction()) {
        auto* fn = static_cast<JSFunction*>(h_obj);
        TaggedValue effective_this = fn->IsArrow() ? fn->GetLexicalThis() : this_val;
        if (fn->IsAsyncGenerator()) {
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* gen_obj = AllocateRuntime<JSGeneratorObject>(fn, args, effective_this, fn->GetEnvironment(), root);
            if (Realm::Current() && Realm::Current()->GetAsyncGeneratorPrototype()) {
                gen_obj->SetPrototype(Realm::Current()->GetAsyncGeneratorPrototype());
            }
            return TaggedValue::FromHeapObject(gen_obj);
        }
        if (fn->IsGenerator()) {
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* gen_obj = AllocateRuntime<JSGeneratorObject>(fn, args, effective_this, fn->GetEnvironment(), root);
            if (Realm::Current() && Realm::Current()->GetGeneratorPrototype()) {
                gen_obj->SetPrototype(Realm::Current()->GetGeneratorPrototype());
            }
            return TaggedValue::FromHeapObject(gen_obj);
        }
        if (fn->IsAsync()) {
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* gen_obj = AllocateRuntime<JSGeneratorObject>(fn, args, effective_this, fn->GetEnvironment(), root);
            auto* promise = AllocateRuntime<JSPromise>(root);
            if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
                promise->SetPrototype(Realm::Current()->GetPromisePrototype());
            }
            gen_obj->SetAsyncPromise(promise);
            if (Interpreter::Current()) {
                Interpreter::Current()->RunAsyncStep(gen_obj, TaggedValue::Undefined(), false);
            }
            return TaggedValue::FromHeapObject(promise);
        }
        if (Interpreter::Current()) {
            TaggedValue prev_target = Interpreter::Current()->GetNewTarget();
            TaggedValue target_to_set = fn->IsArrow() ? fn->GetLexicalNewTarget() : new_target;
            Interpreter::Current()->SetNewTarget(target_to_set);
            TaggedValue res = Interpreter::Current()->Execute(*fn->GetBytecode(), args, effective_this, fn->GetEnvironment());
            Interpreter::Current()->SetNewTarget(prev_target);
            return res;
        }
    }
    return TaggedValue::Undefined();
}

TaggedValue ProxyGet(JSProxy* proxy, std::string_view key, TaggedValue receiver) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'get' on a proxy that has been revoked")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return TaggedValue::Undefined();
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("get");
    if (trap.IsUndefined() || trap.IsNull()) {
        return GetProperty(proxy->GetTarget(), key, receiver.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : receiver);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'get' on proxy: trap is not callable")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
    TaggedValue rec_val = receiver.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : receiver;
    return CallFunction(trap, handler_val, { proxy->GetTarget(), key_val, rec_val });
}

bool ProxySet(JSProxy* proxy, std::string_view key, TaggedValue value, TaggedValue receiver) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'set' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return false;
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("set");
    if (trap.IsUndefined() || trap.IsNull()) {
        return SetProperty(proxy->GetTarget(), key, value, receiver.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : receiver);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'set' on proxy: trap is not callable")));
        }
        return false;
    }
    TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
    TaggedValue rec_val = receiver.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : receiver;
    TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget(), key_val, value, rec_val });
    return res.ToBoolean();
}

bool ProxyHas(JSProxy* proxy, std::string_view key) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'has' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return false;
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("has");
    if (trap.IsUndefined() || trap.IsNull()) {
        return HasProperty(proxy->GetTarget(), key);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'has' on proxy: trap is not callable")));
        }
        return false;
    }
    TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
    TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget(), key_val });
    return res.ToBoolean();
}

bool ProxyDeleteProperty(JSProxy* proxy, std::string_view key) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'deleteProperty' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return false;
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("deleteProperty");
    if (trap.IsUndefined() || trap.IsNull()) {
        return DeleteProperty(proxy->GetTarget(), key);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'deleteProperty' on proxy: trap is not callable")));
        }
        return false;
    }
    TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
    TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget(), key_val });
    return res.ToBoolean();
}

TaggedValue ProxyApply(JSProxy* proxy, TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'apply' on a proxy that has been revoked")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue target_val = proxy->GetTarget();
    if (!target_val.IsHeapObject() || !target_val.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy target is not a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return TaggedValue::Undefined();
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("apply");
    if (trap.IsUndefined() || trap.IsNull()) {
        return CallFunction(target_val, this_val, args);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'apply' on proxy: trap is not callable")));
        }
        return TaggedValue::Undefined();
    }
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* arg_arr = AllocateRuntime<JSArray>(root);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arg_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (const auto& a : args) arg_arr->Push(a);
    return CallFunction(trap, handler_val, { target_val, this_val, TaggedValue::FromHeapObject(arg_arr) });
}

TaggedValue ProxyConstruct(JSProxy* proxy, const std::vector<TaggedValue>& args, TaggedValue new_target) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'construct' on a proxy that has been revoked")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue target_val = proxy->GetTarget();
    if (!target_val.IsHeapObject() || !target_val.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy target is not a constructor")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (!handler_val.IsHeapObject() || !handler_val.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy handler is not an object")));
        }
        return TaggedValue::Undefined();
    }
    auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
    TaggedValue trap = handler->GetNamedProperty("construct");
    if (trap.IsUndefined() || trap.IsNull()) {
        return ConstructObject(target_val, args, new_target.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : new_target);
    }
    if (!trap.IsHeapObject() || !trap.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'construct' on proxy: trap is not callable")));
        }
        return TaggedValue::Undefined();
    }
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* arg_arr = AllocateRuntime<JSArray>(root);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arg_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (const auto& a : args) arg_arr->Push(a);
    TaggedValue nt = new_target.IsEmpty() ? TaggedValue::FromHeapObject(proxy) : new_target;
    TaggedValue res = CallFunction(trap, handler_val, { target_val, TaggedValue::FromHeapObject(arg_arr), nt });
    if (!res.IsHeapObject() || !res.AsHeapObject()->IsJSObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Proxy [[Construct]] must return an object")));
        }
        return TaggedValue::Undefined();
    }
    return res;
}

TaggedValue ProxyGetPrototypeOf(JSProxy* proxy) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'getPrototypeOf' on a proxy that has been revoked")));
        }
        return TaggedValue::Null();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("getPrototypeOf");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            return CallFunction(trap, handler_val, { proxy->GetTarget() });
        }
    }
    return GetPrototypeOf(proxy->GetTarget());
}

bool ProxySetPrototypeOf(JSProxy* proxy, TaggedValue proto) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'setPrototypeOf' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("setPrototypeOf");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget(), proto });
            return res.ToBoolean();
        }
    }
    return SetPrototypeOf(proxy->GetTarget(), proto);
}

TaggedValue ProxyOwnKeys(JSProxy* proxy) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'ownKeys' on a proxy that has been revoked")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("ownKeys");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            return CallFunction(trap, handler_val, { proxy->GetTarget() });
        }
    }
    return GetOwnKeys(proxy->GetTarget());
}

TaggedValue ProxyGetOwnPropertyDescriptor(JSProxy* proxy, std::string_view key) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'getOwnPropertyDescriptor' on a proxy that has been revoked")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("getOwnPropertyDescriptor");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
            return CallFunction(trap, handler_val, { proxy->GetTarget(), key_val });
        }
    }
    if (proxy->GetTarget().IsHeapObject() && proxy->GetTarget().AsHeapObject()->IsJSObject()) {
        auto* tgt = static_cast<JSObject*>(proxy->GetTarget().AsHeapObject());
        if (tgt->HasNamedProperty(key)) {
            TaggedValue val = tgt->GetNamedProperty(key);
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* desc = AllocateRuntime<JSObject>(root);
            uint32_t sid = 9400;
            desc->SetNamedProperty("value", val, sid);
            desc->SetNamedProperty("writable", TaggedValue::FromBoolean(true), sid);
            bool is_enum = (tgt->GetOwnPropertyAttributes(key) & PropertyAttributes::Enumerable) != 0;
            desc->SetNamedProperty("enumerable", TaggedValue::FromBoolean(is_enum), sid);
            desc->SetNamedProperty("configurable", TaggedValue::FromBoolean(true), sid);
            return TaggedValue::FromHeapObject(desc);
        }
    }
    return TaggedValue::Undefined();
}

bool ProxyDefineProperty(JSProxy* proxy, std::string_view key, TaggedValue desc) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'defineProperty' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("defineProperty");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            TaggedValue key_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(key)));
            TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget(), key_val, desc });
            return res.ToBoolean();
        }
    }
    TaggedValue val = TaggedValue::Undefined();
    if (desc.IsHeapObject() && desc.AsHeapObject()->IsJSObject()) {
        val = static_cast<JSObject*>(desc.AsHeapObject())->GetNamedProperty("value");
    }
    return SetProperty(proxy->GetTarget(), key, val);
}

bool ProxyIsExtensible(JSProxy* proxy) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'isExtensible' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("isExtensible");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget() });
            return res.ToBoolean();
        }
    }
    return true;
}

bool ProxyPreventExtensions(JSProxy* proxy) {
    if (proxy->IsRevoked()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot perform 'preventExtensions' on a proxy that has been revoked")));
        }
        return false;
    }
    TaggedValue handler_val = proxy->GetHandler();
    if (handler_val.IsHeapObject() && handler_val.AsHeapObject()->IsJSObject()) {
        auto* handler = static_cast<JSObject*>(handler_val.AsHeapObject());
        TaggedValue trap = handler->GetNamedProperty("preventExtensions");
        if (trap.IsHeapObject() && trap.AsHeapObject()->IsFunction()) {
            TaggedValue res = CallFunction(trap, handler_val, { proxy->GetTarget() });
            return res.ToBoolean();
        }
    }
    return true;
}

std::string ToPropertyKeyString(TaggedValue key_val) {
    if (key_val.IsSymbol()) {
        return static_cast<JSSymbol*>(key_val.AsHeapObject())->Key();
    }
    if (key_val.IsHeapObject() && key_val.AsHeapObject()->IsString()) {
        return static_cast<JSString*>(key_val.AsHeapObject())->Value();
    }
    if (key_val.IsSmi()) {
        return std::to_string(key_val.AsSmi());
    }
    if (key_val.IsHeapObject() && (key_val.AsHeapObject()->IsJSObject() || key_val.AsHeapObject()->IsProxy())) {
        if (Interpreter::Current()) {
            TaggedValue k = Interpreter::Current()->ToPropertyKey(key_val);
            if (k.IsSymbol()) {
                return static_cast<JSSymbol*>(k.AsHeapObject())->Key();
            }
            if (k.IsHeapObject() && k.AsHeapObject()->IsString()) {
                return static_cast<JSString*>(k.AsHeapObject())->Value();
            }
            return k.ToString();
        }
    }
    return key_val.ToString();
}

TaggedValue GetProperty(TaggedValue obj, std::string_view key, TaggedValue receiver) {
    if (!obj.IsHeapObject()) {
        if (obj.IsString() && Realm::Current() && Realm::Current()->GetStringPrototype()) {
            if (key == "length") return TaggedValue::FromSmi(static_cast<int32_t>(obj.ToString().length()));
            return Realm::Current()->GetStringPrototype()->GetNamedProperty(key);
        }
        if (obj.IsNumber() && Realm::Current() && Realm::Current()->GetNumberPrototype()) {
            return Realm::Current()->GetNumberPrototype()->GetNamedProperty(key);
        }
        if (obj.IsBoolean() && Realm::Current() && Realm::Current()->GetBooleanPrototype()) {
            return Realm::Current()->GetBooleanPrototype()->GetNamedProperty(key);
        }
        return TaggedValue::Undefined();
    }
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) {
        return ProxyGet(static_cast<JSProxy*>(ho), key, receiver.IsEmpty() ? obj : receiver);
    }
    if (ho->IsString()) {
        auto* str = static_cast<JSString*>(ho);
        if (key == "length") return TaggedValue::FromSmi(static_cast<int32_t>(str->Length()));
        if (Realm::Current() && Realm::Current()->GetStringPrototype()) {
            return Realm::Current()->GetStringPrototype()->GetNamedProperty(key);
        }
        return TaggedValue::Undefined();
    }
    if (ho->IsJSObject()) {
        return static_cast<JSObject*>(ho)->GetNamedProperty(key);
    }
    return TaggedValue::Undefined();
}

bool SetProperty(TaggedValue obj, std::string_view key, TaggedValue val, TaggedValue receiver, uint32_t* next_shape_id) {
    if (!obj.IsHeapObject()) return false;
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) {
        return ProxySet(static_cast<JSProxy*>(ho), key, val, receiver.IsEmpty() ? obj : receiver);
    }
    if (ho->IsJSObject()) {
        uint32_t default_sid = 9000;
        uint32_t& sid = next_shape_id ? *next_shape_id : default_sid;
        static_cast<JSObject*>(ho)->SetNamedProperty(key, val, sid);
        return true;
    }
    return false;
}

bool HasProperty(TaggedValue obj, std::string_view key) {
    if (!obj.IsHeapObject()) return false;
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) {
        return ProxyHas(static_cast<JSProxy*>(ho), key);
    }
    if (ho->IsArray()) {
        auto* arr = static_cast<JSArray*>(ho);
        if (key == "length") return true;
        try {
            size_t idx = std::stoull(std::string(key));
            if (idx < arr->Length()) {
                return !arr->Elements()[idx].IsEmpty();
            }
        } catch (...) {}
    }
    if (ho->IsJSObject()) {
        return static_cast<JSObject*>(ho)->HasNamedProperty(key);
    }
    if (ho->IsString()) {
        if (key == "length") return true;
    }
    return false;
}

bool DeleteProperty(TaggedValue obj, std::string_view key) {
    if (!obj.IsHeapObject()) return true;
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) {
        return ProxyDeleteProperty(static_cast<JSProxy*>(ho), key);
    }
    if (ho->IsJSObject()) {
        return static_cast<JSObject*>(ho)->DeleteNamedProperty(key);
    }
    return true;
}

TaggedValue GetPrototypeOf(TaggedValue obj) {
    if (!obj.IsHeapObject()) return TaggedValue::Null();
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) return ProxyGetPrototypeOf(static_cast<JSProxy*>(ho));
    if (ho->IsJSObject()) {
        JSObject* p = static_cast<JSObject*>(ho)->GetPrototype();
        return p ? TaggedValue::FromHeapObject(p) : TaggedValue::Null();
    }
    return TaggedValue::Null();
}

bool SetPrototypeOf(TaggedValue obj, TaggedValue proto) {
    if (!obj.IsHeapObject()) return false;
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) return ProxySetPrototypeOf(static_cast<JSProxy*>(ho), proto);
    if (ho->IsJSObject()) {
        JSObject* p = (proto.IsHeapObject() && proto.AsHeapObject()->IsJSObject()) ? static_cast<JSObject*>(proto.AsHeapObject()) : nullptr;
        return static_cast<JSObject*>(ho)->SetPrototype(p);
    }
    return false;
}

TaggedValue GetOwnKeys(TaggedValue obj) {
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* arr = AllocateRuntime<JSArray>(root);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    if (!obj.IsHeapObject()) return TaggedValue::FromHeapObject(arr);
    HeapObject* ho = obj.AsHeapObject();
    if (ho->IsProxy()) return ProxyOwnKeys(static_cast<JSProxy*>(ho));
    if (ho->IsJSObject()) {
        auto names = static_cast<JSObject*>(ho)->GetOwnPropertyNames();
        for (const auto& name : names) {
            arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(name)));
        }
    }
    return TaggedValue::FromHeapObject(arr);
}

TaggedValue ConstructObject(TaggedValue ctor, const std::vector<TaggedValue>& args, TaggedValue new_target) {
    if (!ctor.IsHeapObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Value is not a constructor")));
        }
        return TaggedValue::Undefined();
    }
    HeapObject* ho = ctor.AsHeapObject();
    if (ho->IsProxy()) {
        return ProxyConstruct(static_cast<JSProxy*>(ho), args, new_target);
    }
    TaggedValue target = new_target.IsEmpty() ? ctor : new_target;
    if (ho->IsNativeFunction()) {
        auto* nfn = static_cast<JSNativeFunction*>(ho);
        if (!nfn->IsConstructor()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Function is not a constructor")));
            }
            return TaggedValue::Undefined();
        }
        return CallFunction(ctor, TaggedValue::Undefined(), args, target);
    }
    if (ho->IsUserFunction()) {
        auto* fn = static_cast<JSFunction*>(ho);
        if (fn->IsArrow() || fn->IsAsync() || fn->IsGenerator()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function is not a constructor")));
            }
            return TaggedValue::Undefined();
        }
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* instance = AllocateRuntime<JSObject>(root);
        JSObject* proto = fn->GetOrCreateConstructorPrototype();
        if (proto != nullptr) {
            instance->SetPrototype(proto);
        } else if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
            instance->SetPrototype(Realm::Current()->GetObjectPrototype());
        }
        TaggedValue inst_val = TaggedValue::FromHeapObject(instance);
        uint32_t field_sid = 11000;
        for (const auto& f : fn->InstanceFields()) {
            std::string property = f.name.size() > 0 && f.name[0] == '#'
                ? "__private__" + f.name.substr(1) : f.name;
            instance->SetNamedProperty(property, f.initial_value, field_sid);
            if (f.is_method) {
                instance->SetOwnPropertyAttributes(property, PropertyAttributes::None);
            }
        }
        TaggedValue ret = CallFunction(ctor, inst_val, args, target);
        if (ret.IsHeapObject() && ret.AsHeapObject()->IsJSObject()) {
            return ret;
        }
        return inst_val;
    }
    if (Interpreter::Current()) {
        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
            AllocateRuntime<JSError>("TypeError", "Value is not a constructor")));
    }
    return TaggedValue::Undefined();
}

JSObject* CreateIterResult(TaggedValue value, bool done) {
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* obj = AllocateRuntime<JSObject>(root);
    uint32_t sid = 5000;
    obj->SetNamedProperty("value", value, sid);
    obj->SetNamedProperty("done", TaggedValue::FromBoolean(done), sid);
    return obj;
}

TaggedValue OrdinaryHasInstance(TaggedValue C, TaggedValue O) {
    if (!C.IsHeapObject()) return TaggedValue::FromBoolean(false);
    HeapObject* c_ho = C.AsHeapObject();
    bool c_callable = c_ho->IsFunction() || (c_ho->IsProxy() && !static_cast<JSProxy*>(c_ho)->IsRevoked());
    if (!c_callable) {
        return TaggedValue::FromBoolean(false);
    }

    if (c_ho->IsJSObject()) {
        TaggedValue bound_target = static_cast<JSObject*>(c_ho)->GetNamedProperty("__bound_target__");
        if (!bound_target.IsEmpty() && !bound_target.IsUndefined()) {
            return InstanceofOperator(O, bound_target);
        }
    }

    if (!O.IsHeapObject() || !O.AsHeapObject()->IsECMAObject()) {
        return TaggedValue::FromBoolean(false);
    }

    TaggedValue P = GetProperty(C, "prototype");
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
        return TaggedValue::Undefined();
    }

    if (!P.IsHeapObject() || !P.AsHeapObject()->IsECMAObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Function has non-object prototype in instanceof check")));
        }
        return TaggedValue::Undefined();
    }

    TaggedValue curr = GetPrototypeOf(O);
    while (!curr.IsNull() && curr.IsHeapObject()) {
        if (curr == P) {
            return TaggedValue::FromBoolean(true);
        }
        curr = GetPrototypeOf(curr);
    }
    return TaggedValue::FromBoolean(false);
}

TaggedValue InstanceofOperator(TaggedValue V, TaggedValue target) {
    if (!target.IsHeapObject() || !target.AsHeapObject()->IsECMAObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Right-hand side of 'instanceof' is not an object")));
        }
        return TaggedValue::Undefined();
    }

    TaggedValue sym_has_inst = Realm::Current() ? Realm::Current()->GetSymbolHasInstance() : TaggedValue::Undefined();
    std::string sym_key = (sym_has_inst.IsSymbol()) ? static_cast<JSSymbol*>(sym_has_inst.AsHeapObject())->Key() : "@@hasInstance";

    TaggedValue has_instance = GetProperty(target, sym_key);
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
        return TaggedValue::Undefined();
    }

    if (!has_instance.IsUndefined() && !has_instance.IsNull()) {
        bool callable = has_instance.IsHeapObject() &&
                        (has_instance.AsHeapObject()->IsFunction() ||
                         (has_instance.AsHeapObject()->IsProxy() && !static_cast<JSProxy*>(has_instance.AsHeapObject())->IsRevoked()));
        if (!callable) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Symbol.hasInstance is not a function")));
            }
            return TaggedValue::Undefined();
        }

        TaggedValue res = CallFunction(has_instance, target, {V});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromBoolean(res.ToBoolean());
    }

    bool target_callable = target.AsHeapObject()->IsFunction() ||
                           (target.AsHeapObject()->IsProxy() && !static_cast<JSProxy*>(target.AsHeapObject())->IsRevoked());
    if (!target_callable) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Right-hand side of 'instanceof' is not callable")));
        }
        return TaggedValue::Undefined();
    }

    return OrdinaryHasInstance(target, V);
}

// Math Builtins
static TaggedValue Math_abs(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    TaggedValue val = args[0];
    if (val.IsSmi()) {
        int32_t s = val.AsSmi();
        return TaggedValue::FromSmi(s < 0 ? -s : s);
    }
    double d = val.AsNumber();
    return TaggedValue::FromDouble(std::abs(d));
}

static TaggedValue Math_floor(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::floor(d));
}

static TaggedValue Math_ceil(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::ceil(d));
}

static TaggedValue Math_round(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    double val = args[0].ToDouble();
    if (std::isnan(val)) return TaggedValue::FromDouble(val);
    double res = std::floor(val + 0.5);
    if (res >= -1073741824.0 && res <= 1073741823.0 && (res != 0.0 || !std::signbit(res))) {
        return TaggedValue::FromSmi(static_cast<int32_t>(res));
    }
    return TaggedValue::FromDouble(res);
}

static TaggedValue Math_sqrt(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::nan(""));
    double d = args[0].AsNumber();
    return TaggedValue::FromDouble(std::sqrt(d));
}

static TaggedValue Math_max(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(-std::numeric_limits<double>::infinity());
    bool all_smi = true;
    for (const auto& a : args) {
        if (!a.IsSmi()) { all_smi = false; break; }
    }
    if (all_smi) {
        int32_t max_val = args[0].AsSmi();
        for (size_t i = 1; i < args.size(); ++i) {
            max_val = std::max(max_val, args[i].AsSmi());
        }
        return TaggedValue::FromSmi(max_val);
    }
    double max_val = args[0].AsNumber();
    for (size_t i = 1; i < args.size(); ++i) {
        max_val = std::max(max_val, args[i].AsNumber());
    }
    return TaggedValue::FromDouble(max_val);
}

static TaggedValue Math_min(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::infinity());
    bool all_smi = true;
    for (const auto& a : args) {
        if (!a.IsSmi()) { all_smi = false; break; }
    }
    if (all_smi) {
        int32_t min_val = args[0].AsSmi();
        for (size_t i = 1; i < args.size(); ++i) {
            min_val = std::min(min_val, args[i].AsSmi());
        }
        return TaggedValue::FromSmi(min_val);
    }
    double min_val = args[0].AsNumber();
    for (size_t i = 1; i < args.size(); ++i) {
        min_val = std::min(min_val, args[i].AsNumber());
    }
    return TaggedValue::FromDouble(min_val);
}

static TaggedValue Math_pow(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    double base = args[0].ToDouble();
    double exp = (args.size() > 1) ? args[1].ToDouble() : std::numeric_limits<double>::quiet_NaN();
    if (std::isnan(exp)) {
        return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    } else if (exp == 0.0) {
        return TaggedValue::FromDouble(1.0);
    } else if (std::isnan(base)) {
        return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    } else if (std::abs(base) == 1.0 && std::isinf(exp)) {
        return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    }
    return TaggedValue::FromDouble(std::pow(base, exp));
}

static TaggedValue Math_exp(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::exp(args[0].ToDouble()));
}

static TaggedValue Math_expm1(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::expm1(args[0].ToDouble()));
}

static TaggedValue Math_log(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::log(args[0].ToDouble()));
}

static TaggedValue Math_log10(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::log10(args[0].ToDouble()));
}

static TaggedValue Math_log2(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::log2(args[0].ToDouble()));
}

static TaggedValue Math_log1p(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::log1p(args[0].ToDouble()));
}

static TaggedValue Math_sin(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::sin(args[0].ToDouble()));
}

static TaggedValue Math_cos(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::cos(args[0].ToDouble()));
}

static TaggedValue Math_tan(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::tan(args[0].ToDouble()));
}

static TaggedValue Math_asin(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::asin(args[0].ToDouble()));
}

static TaggedValue Math_acos(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::acos(args[0].ToDouble()));
}

static TaggedValue Math_atan(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::atan(args[0].ToDouble()));
}

static TaggedValue Math_atan2(const std::vector<TaggedValue>& args) {
    if (args.size() < 2) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::atan2(args[0].ToDouble(), args[1].ToDouble()));
}

static TaggedValue Math_sinh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::sinh(args[0].ToDouble()));
}

static TaggedValue Math_cosh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::cosh(args[0].ToDouble()));
}

static TaggedValue Math_tanh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::tanh(args[0].ToDouble()));
}

static TaggedValue Math_asinh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::asinh(args[0].ToDouble()));
}

static TaggedValue Math_acosh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::acosh(args[0].ToDouble()));
}

static TaggedValue Math_atanh(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::atanh(args[0].ToDouble()));
}

static TaggedValue Math_cbrt(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::cbrt(args[0].ToDouble()));
}

static TaggedValue Math_hypot(const std::vector<TaggedValue>& args) {
    double sum = 0.0;
    for (const auto& a : args) {
        double v = a.ToDouble();
        sum += v * v;
    }
    return TaggedValue::FromDouble(std::sqrt(sum));
}

static TaggedValue Math_trunc(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(std::trunc(args[0].ToDouble()));
}

static TaggedValue Math_sign(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    double x = args[0].ToDouble();
    if (std::isnan(x) || x == 0.0) return TaggedValue::FromDouble(x);
    return TaggedValue::FromDouble(x > 0.0 ? 1.0 : -1.0);
}

static TaggedValue Math_clz32(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromSmi(32);
    uint32_t val = static_cast<uint32_t>(static_cast<int64_t>(args[0].ToDouble()));
    if (val == 0) return TaggedValue::FromSmi(32);
    int leading_zeros = 0;
    for (int i = 31; i >= 0; --i) {
        if ((val & (1u << i)) != 0) break;
        leading_zeros++;
    }
    return TaggedValue::FromSmi(leading_zeros);
}

static TaggedValue Math_imul(const std::vector<TaggedValue>& args) {
    int32_t a = args.empty() ? 0 : static_cast<int32_t>(static_cast<int64_t>(args[0].ToDouble()));
    int32_t b = args.size() < 2 ? 0 : static_cast<int32_t>(static_cast<int64_t>(args[1].ToDouble()));
    return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<uint32_t>(a) * static_cast<uint32_t>(b)));
}

static TaggedValue Math_fround(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(static_cast<double>(static_cast<float>(args[0].ToDouble())));
}

static TaggedValue Math_random(const std::vector<TaggedValue>&) {
    static std::mt19937_64 rng(std::random_device{}());
    static std::uniform_real_distribution<double> dist(0.0, 1.0);
    return TaggedValue::FromDouble(dist(rng));
}

// Console Builtins
static TaggedValue Console_log(const std::vector<TaggedValue>& args) {
    for (size_t i = 0; i < args.size(); ++i) {
        if (i > 0) std::cout << " ";
        std::cout << args[i].ToString();
    }
    std::cout << "\n";
    return TaggedValue::Undefined();
}

// Array Prototype Methods & Helpers
static bool StrictEqualValues(TaggedValue a, TaggedValue b) {
    if (a.IsSmi() && b.IsSmi()) return a.AsSmi() == b.AsSmi();
    if (a.IsNumber() && b.IsNumber()) return a.AsNumber() == b.AsNumber();
    if (a.IsHeapObject() && b.IsHeapObject() && a.AsHeapObject()->IsString() && b.AsHeapObject()->IsString()) {
        return static_cast<JSString*>(a.AsHeapObject())->View() == static_cast<JSString*>(b.AsHeapObject())->View();
    }
    if (a.IsHeapObject() && b.IsHeapObject() && a.AsHeapObject()->IsBigInt() && b.AsHeapObject()->IsBigInt()) {
        return static_cast<JSBigInt*>(a.AsHeapObject())->Equals(*static_cast<JSBigInt*>(b.AsHeapObject()));
    }
    return a == b;
}

static bool SameValue(TaggedValue x, TaggedValue y) {
    if (x.IsNumber() && y.IsNumber()) {
        double dx = x.AsNumber();
        double dy = y.AsNumber();
        if (std::isnan(dx) && std::isnan(dy)) return true;
        if (dx == 0.0 && dy == 0.0) {
            return std::signbit(dx) == std::signbit(dy);
        }
        return dx == dy;
    }
    return StrictEqualValues(x, y);
}

static uint64_t ToLength(TaggedValue v) {
    double d = v.ToDouble();
    if (std::isnan(d) || d <= 0.0) return 0;
    if (d >= 9007199254740991.0) return 9007199254740991ULL; // 2^53 - 1
    return static_cast<uint64_t>(d);
}

static JSObject* ToObject(TaggedValue val) {
    if (val.IsNull() || val.IsUndefined()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
        }
        return nullptr;
    }
    if (val.IsHeapObject()) {
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsJSObject()) return static_cast<JSObject*>(ho);
        if (ho->IsString()) {
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue str_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("String");
                if (str_ctor.IsHeapObject() && str_ctor.AsHeapObject()->IsFunction()) {
                    TaggedValue obj = ConstructObject(str_ctor, {val});
                    if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
                }
            }
        }
        if (ho->IsBigInt()) {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue bigint_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("BigInt");
                if (bigint_ctor.IsHeapObject() && bigint_ctor.AsHeapObject()->IsJSObject()) {
                    TaggedValue proto = static_cast<JSObject*>(bigint_ctor.AsHeapObject())->GetNamedProperty("prototype");
                    if (proto.IsHeapObject() && proto.AsHeapObject()->IsJSObject()) {
                        obj->SetPrototype(static_cast<JSObject*>(proto.AsHeapObject()));
                    }
                }
            }
            uint32_t next_sid = 0;
            obj->SetNamedProperty("__bigint_data__", val, next_sid);
            return obj;
        }
    } else if (val.IsSmi() || val.IsDouble()) {
        if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
            TaggedValue num_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Number");
            if (num_ctor.IsHeapObject() && num_ctor.AsHeapObject()->IsFunction()) {
                TaggedValue obj = ConstructObject(num_ctor, {val});
                if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
            }
        }
    } else if (val.IsBoolean()) {
        if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
            TaggedValue bool_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Boolean");
            if (bool_ctor.IsHeapObject() && bool_ctor.AsHeapObject()->IsFunction()) {
                TaggedValue obj = ConstructObject(bool_ctor, {val});
                if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
            }
        }
    } else if (val.IsSymbol()) {
        auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
            TaggedValue sym_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Symbol");
            if (sym_ctor.IsHeapObject() && sym_ctor.AsHeapObject()->IsJSObject()) {
                TaggedValue proto = static_cast<JSObject*>(sym_ctor.AsHeapObject())->GetNamedProperty("prototype");
                if (proto.IsHeapObject() && proto.AsHeapObject()->IsJSObject()) {
                    obj->SetPrototype(static_cast<JSObject*>(proto.AsHeapObject()));
                }
            }
        }
        uint32_t next_sid = 0;
        obj->SetNamedProperty("__symbol_data__", val, next_sid);
        return obj;
    }
    return nullptr;
}

static bool DefinePropertyOnObject(JSObject* obj, std::string_view key, TaggedValue desc) {
    if (!desc.IsHeapObject() || !desc.AsHeapObject()->IsECMAObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Property description must be an object")));
        }
        return false;
    }
    bool has_get = HasProperty(desc, "get");
    bool has_set = HasProperty(desc, "set");
    bool has_val = HasProperty(desc, "value");
    bool has_wri = HasProperty(desc, "writable");
    bool has_enu = HasProperty(desc, "enumerable");
    bool has_cnf = HasProperty(desc, "configurable");

    TaggedValue get_fn = has_get ? GetProperty(desc, "get") : TaggedValue::Undefined();
    TaggedValue set_fn = has_set ? GetProperty(desc, "set") : TaggedValue::Undefined();

    if (has_get && !get_fn.IsUndefined()) {
        if (!get_fn.IsHeapObject() || !get_fn.AsHeapObject()->IsFunction()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Getter must be a function")));
            }
            return false;
        }
    }
    if (has_set && !set_fn.IsUndefined()) {
        if (!set_fn.IsHeapObject() || !set_fn.AsHeapObject()->IsFunction()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Setter must be a function")));
            }
            return false;
        }
    }
    if ((has_get || has_set) && (has_val || has_wri)) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Invalid property descriptor: cannot specify both value/writable and get/set")));
        }
        return false;
    }

    bool enu = has_enu ? GetProperty(desc, "enumerable").ToBoolean() : false;
    bool cnf = has_cnf ? GetProperty(desc, "configurable").ToBoolean() : false;
    bool wri = has_wri ? GetProperty(desc, "writable").ToBoolean() : false;
    TaggedValue val = has_val ? GetProperty(desc, "value") : TaggedValue::Undefined();

    if (obj->IsArray() && key == "length") {
        if (has_get || has_set) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Array length cannot be accessor")));
            return false;
        }
        if (has_cnf && cnf) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot make array length configurable")));
            return false;
        }
        if (has_enu && enu) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot make array length enumerable")));
            return false;
        }
        auto* arr = static_cast<JSArray*>(obj);
        PropertyAttributes cur_attrs = arr->GetOwnPropertyAttributes("length");
        if (has_val) {
            double d = val.ToDouble();
            uint32_t new_len = static_cast<uint32_t>(d);
            if (std::isnan(d) || static_cast<double>(new_len) != d) {
                if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("RangeError", "Invalid array length")));
                return false;
            }
            if (!(cur_attrs & PropertyAttributes::Writable) && new_len != arr->Length()) {
                if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot change length of non-writable array")));
                return false;
            }
            arr->SetLength(new_len);
        }
        if (has_wri && !wri) {
            arr->SetOwnPropertyAttributes("length", PropertyAttributes::None);
        }
        return true;
    }

    bool current_exists = obj->HasOwnProperty(key);
    if (!current_exists) {
        if (!obj->IsExtensible()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot define property on non-extensible object")));
            }
            return false;
        }
        if (has_get || has_set) {
            PropertyAttributes attrs = (enu ? PropertyAttributes::Enumerable : PropertyAttributes::None) |
                                       (cnf ? PropertyAttributes::Configurable : PropertyAttributes::None);
            obj->SetAccessor(key, get_fn, set_fn, attrs);
            return true;
        }
        PropertyAttributes attrs = (wri ? PropertyAttributes::Writable : PropertyAttributes::None) |
                                   (enu ? PropertyAttributes::Enumerable : PropertyAttributes::None) |
                                   (cnf ? PropertyAttributes::Configurable : PropertyAttributes::None);
        uint32_t sid = 0;
        obj->SetNamedProperty(key, val, sid);
        obj->SetOwnPropertyAttributes(key, attrs);
        return true;
    }

    PropertyAttributes cur_attrs = obj->GetOwnPropertyAttributes(key);
    bool cur_is_accessor = obj->HasAccessor(key);
    bool cur_cnf = (cur_attrs & PropertyAttributes::Configurable);
    bool cur_enu = (cur_attrs & PropertyAttributes::Enumerable);
    bool cur_wri = (cur_attrs & PropertyAttributes::Writable);

    if (!cur_cnf) {
        if (has_cnf && cnf) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot redefine non-configurable property")));
            return false;
        }
        if (has_enu && enu != cur_enu) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot change enumerable attribute of non-configurable property")));
            return false;
        }
        if ((has_get || has_set) && !cur_is_accessor) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot change data property to accessor on non-configurable property")));
            return false;
        }
        if ((has_val || has_wri) && cur_is_accessor) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot change accessor property to data on non-configurable property")));
            return false;
        }
        if (cur_is_accessor) {
            const auto* acc = obj->GetAccessor(key);
            TaggedValue cur_get = acc ? acc->getter : TaggedValue::Undefined();
            TaggedValue cur_set = acc ? acc->setter : TaggedValue::Undefined();
            if (has_get && !SameValue(get_fn, cur_get)) {
                if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot modify getter of non-configurable accessor")));
                return false;
            }
            if (has_set && !SameValue(set_fn, cur_set)) {
                if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot modify setter of non-configurable accessor")));
                return false;
            }
        } else {
            if (!cur_wri) {
                if (has_wri && wri) {
                    if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot make non-writable property writable on non-configurable property")));
                    return false;
                }
                if (has_val) {
                    TaggedValue cur_val = obj->GetNamedProperty(key);
                    if (!SameValue(val, cur_val)) {
                        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot change value of non-writable non-configurable property")));
                        return false;
                    }
                }
            }
        }
    }

    if (has_get || has_set || (cur_is_accessor && !has_val && !has_wri)) {
        TaggedValue cur_get = TaggedValue::Undefined();
        TaggedValue cur_set = TaggedValue::Undefined();
        if (cur_is_accessor) {
            const auto* acc = obj->GetAccessor(key);
            if (acc) { cur_get = acc->getter; cur_set = acc->setter; }
        }
        TaggedValue final_get = has_get ? get_fn : cur_get;
        TaggedValue final_set = has_set ? set_fn : cur_set;
        bool final_enu = has_enu ? enu : cur_enu;
        bool final_cnf = has_cnf ? cnf : cur_cnf;
        PropertyAttributes final_attrs = (final_enu ? PropertyAttributes::Enumerable : PropertyAttributes::None) |
                                         (final_cnf ? PropertyAttributes::Configurable : PropertyAttributes::None);
        obj->SetAccessor(key, final_get, final_set, final_attrs);
        return true;
    } else {
        if (cur_is_accessor) {
            obj->RemoveAccessor(key);
        }
        TaggedValue final_val = has_val ? val : obj->GetNamedProperty(key);
        bool final_wri = has_wri ? wri : cur_wri;
        bool final_enu = has_enu ? enu : cur_enu;
        bool final_cnf = has_cnf ? cnf : cur_cnf;
        PropertyAttributes final_attrs = (final_wri ? PropertyAttributes::Writable : PropertyAttributes::None) |
                                         (final_enu ? PropertyAttributes::Enumerable : PropertyAttributes::None) |
                                         (final_cnf ? PropertyAttributes::Configurable : PropertyAttributes::None);
        uint32_t sid = 0;
        obj->SetNamedProperty(key, final_val, sid);
        obj->SetOwnPropertyAttributes(key, final_attrs);
        return true;
    }
}

static TaggedValue ObjectDefineProperties(TaggedValue target_val, TaggedValue props_val) {
    if (!target_val.IsHeapObject() || !target_val.AsHeapObject()->IsECMAObject()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Object.defineProperties called on non-object")));
        }
        return TaggedValue::Undefined();
    }
    JSObject* props = ToObject(props_val);
    if (!props) {
        return TaggedValue::Undefined();
    }
    TaggedValue keys_val = GetOwnKeys(TaggedValue::FromHeapObject(props));
    if (!keys_val.IsHeapObject() || !keys_val.AsHeapObject()->IsArray()) {
        return target_val;
    }
    auto* keys_arr = static_cast<JSArray*>(keys_val.AsHeapObject());
    struct PropEntry {
        std::string key;
        TaggedValue desc;
    };
    std::vector<PropEntry> descriptors;
    for (size_t i = 0; i < keys_arr->Length(); ++i) {
        std::string key = keys_arr->GetElement(i).ToString();
        bool is_enumerable = false;
        if (props->IsProxy()) {
            TaggedValue d = ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(props), key);
            if (d.IsHeapObject() && d.AsHeapObject()->IsJSObject()) {
                TaggedValue enum_val = static_cast<JSObject*>(d.AsHeapObject())->GetNamedProperty("enumerable");
                is_enumerable = enum_val.IsUndefined() || enum_val.ToBoolean();
            } else {
                is_enumerable = true;
            }
        } else {
            is_enumerable = (props->GetOwnPropertyAttributes(key) & PropertyAttributes::Enumerable) != 0;
        }
        if (is_enumerable) {
            TaggedValue desc_obj = GetProperty(TaggedValue::FromHeapObject(props), key);
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                return TaggedValue::Undefined();
            }
            if (!desc_obj.IsHeapObject() || !desc_obj.AsHeapObject()->IsECMAObject()) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Property description must be an object")));
                }
                return TaggedValue::Undefined();
            }
            bool has_accessor = HasProperty(desc_obj, "get") || HasProperty(desc_obj, "set");
            bool has_data = HasProperty(desc_obj, "value") || HasProperty(desc_obj, "writable");
            if (has_accessor && has_data) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Invalid property descriptor: cannot specify both value/writable and get/set")));
                }
                return TaggedValue::Undefined();
            }
            descriptors.push_back({std::move(key), desc_obj});
        }
    }
    for (const auto& entry : descriptors) {
        if (target_val.AsHeapObject()->IsProxy()) {
            ProxyDefineProperty(static_cast<JSProxy*>(target_val.AsHeapObject()), entry.key, entry.desc);
        } else {
            if (!DefinePropertyOnObject(static_cast<JSObject*>(target_val.AsHeapObject()), entry.key, entry.desc)) {
                return TaggedValue::Undefined();
            }
        }
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            return TaggedValue::Undefined();
        }
    }
    return target_val;
}

static TaggedValue Array_push(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        for (const auto& a : args) arr->Push(a);
        uint64_t new_len = arr->Length();
        return new_len <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(new_len)) : TaggedValue::FromDouble(static_cast<double>(new_len));
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    if (len + args.size() > 9007199254740991ULL) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Array push length exceeds 2^53 - 1")));
        return TaggedValue::Undefined();
    }
    for (const auto& a : args) {
        SetProperty(TaggedValue::FromHeapObject(obj), std::to_string(len), a);
        len++;
    }
    uint32_t sid = 0;
    TaggedValue len_tv = len <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(len)) : TaggedValue::FromDouble(static_cast<double>(len));
    obj->SetNamedProperty("length", len_tv, sid);
    return len_tv;
}

static TaggedValue Array_pop(TaggedValue this_val, const std::vector<TaggedValue>&) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        return arr->Pop();
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    if (len == 0) {
        uint32_t sid = 0;
        obj->SetNamedProperty("length", TaggedValue::FromSmi(0), sid);
        return TaggedValue::Undefined();
    }
    len--;
    std::string idx_str = std::to_string(len);
    TaggedValue element = GetProperty(TaggedValue::FromHeapObject(obj), idx_str);
    obj->DeleteNamedProperty(idx_str);
    uint32_t sid = 0;
    obj->SetNamedProperty("length", TaggedValue::FromDouble(static_cast<double>(len)), sid);
    return element;
}

static TaggedValue Array_shift(TaggedValue this_val, const std::vector<TaggedValue>&) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        if (arr->Length() == 0) return TaggedValue::Undefined();
        TaggedValue first = arr->GetElement(0);
        arr->Elements().erase(arr->Elements().begin());
        return first;
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    if (len == 0) {
        uint32_t sid = 0;
        obj->SetNamedProperty("length", TaggedValue::FromSmi(0), sid);
        return TaggedValue::Undefined();
    }
    TaggedValue first = GetProperty(TaggedValue::FromHeapObject(obj), "0");
    for (uint64_t k = 1; k < len; ++k) {
        std::string from = std::to_string(k);
        std::string to = std::to_string(k - 1);
        if (HasProperty(TaggedValue::FromHeapObject(obj), from)) {
            SetProperty(TaggedValue::FromHeapObject(obj), to, GetProperty(TaggedValue::FromHeapObject(obj), from));
        } else {
            obj->DeleteNamedProperty(to);
        }
    }
    obj->DeleteNamedProperty(std::to_string(len - 1));
    uint32_t sid = 0;
    obj->SetNamedProperty("length", TaggedValue::FromDouble(static_cast<double>(len - 1)), sid);
    return first;
}

static TaggedValue Array_unshift(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        arr->Elements().insert(arr->Elements().begin(), args.begin(), args.end());
        if (arr->IsOld()) {
            for (const auto& arg : args) {
                if (arg.IsHeapObject()) RecordWriteBarrier(arr, arg);
            }
        }
        uint64_t new_len = arr->Length();
        return new_len <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(new_len)) : TaggedValue::FromDouble(static_cast<double>(new_len));
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    uint64_t arg_count = args.size();
    if (len + arg_count > 9007199254740991ULL) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Array unshift length exceeds 2^53 - 1")));
        return TaggedValue::Undefined();
    }
    if (arg_count > 0) {
        for (uint64_t k = len; k > 0; --k) {
            std::string from = std::to_string(k - 1);
            std::string to = std::to_string(k + arg_count - 1);
            if (HasProperty(TaggedValue::FromHeapObject(obj), from)) {
                SetProperty(TaggedValue::FromHeapObject(obj), to, GetProperty(TaggedValue::FromHeapObject(obj), from));
            } else {
                obj->DeleteNamedProperty(to);
            }
        }
        for (uint64_t j = 0; j < arg_count; ++j) {
            SetProperty(TaggedValue::FromHeapObject(obj), std::to_string(j), args[j]);
        }
    }
    uint64_t new_len = len + arg_count;
    uint32_t sid = 0;
    TaggedValue len_tv = new_len <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(new_len)) : TaggedValue::FromDouble(static_cast<double>(new_len));
    obj->SetNamedProperty("length", len_tv, sid);
    return len_tv;
}

static TaggedValue Array_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    int64_t len = obj->IsArray() ? static_cast<int64_t>(static_cast<JSArray*>(obj)->Length()) : static_cast<int64_t>(ToLength(obj->GetNamedProperty("length")));
    int64_t start = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble())) : 0;
    int64_t end = args.size() > 1 && !args[1].IsUndefined() ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].ToDouble())) : len;

    if (start < 0) start = std::max<int64_t>(0, len + start);
    else start = std::min<int64_t>(len, start);

    if (end < 0) end = std::max<int64_t>(0, len + end);
    else end = std::min<int64_t>(len, end);

    std::vector<TaggedValue> sliced;
    if (start < end) {
        sliced.reserve(static_cast<size_t>(end - start));
        for (int64_t i = start; i < end; ++i) {
            if (obj->IsArray()) {
                sliced.push_back(static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(i)));
            } else {
                sliced.push_back(GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(i)));
            }
        }
    }
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, std::move(sliced));
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Object_toString(TaggedValue this_val, const std::vector<TaggedValue>& args);

static TaggedValue Array_join(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    std::string sep = (args.size() > 0 && !args[0].IsUndefined()) ? args[0].ToString() : ",";
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    std::string res;
    for (uint64_t i = 0; i < len; ++i) {
        if (i > 0) res += sep;
        TaggedValue el = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(i)) : GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(i));
        if (!el.IsUndefined() && !el.IsNull()) {
            res += el.ToString();
        }
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

static TaggedValue Array_toString(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject()) {
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("[object Object]"));
    }
    TaggedValue join_fn = TaggedValue::Undefined();
    if (this_val.AsHeapObject()->IsJSObject()) {
        join_fn = static_cast<JSObject*>(this_val.AsHeapObject())->GetNamedProperty("join");
    }
    if (join_fn.IsHeapObject() && (join_fn.AsHeapObject()->IsFunction() || join_fn.AsHeapObject()->IsProxy())) {
        return CallFunction(join_fn, this_val, {});
    }
    return Object_toString(this_val, args);
}

static TaggedValue Array_toLocaleString(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    return Array_toString(this_val, args);
}

static TaggedValue Array_indexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::FromSmi(-1);
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    TaggedValue search = args.empty() ? TaggedValue::Undefined() : args[0];
    int64_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<int64_t>(args[1].AsSmi()) : (args.size() > 1 ? static_cast<int64_t>(args[1].ToDouble()) : 0);
    if (from < 0) from = std::max<int64_t>(0, static_cast<int64_t>(len) + from);
    for (uint64_t i = static_cast<uint64_t>(from); i < len; ++i) {
        TaggedValue el = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(i)) : GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(i));
        if (StrictEqualValues(el, search)) {
            return i <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(i)) : TaggedValue::FromDouble(static_cast<double>(i));
        }
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue Array_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::FromBoolean(false);
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    TaggedValue search = args.empty() ? TaggedValue::Undefined() : args[0];
    int64_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<int64_t>(args[1].AsSmi()) : (args.size() > 1 ? static_cast<int64_t>(args[1].ToDouble()) : 0);
    if (from < 0) from = std::max<int64_t>(0, static_cast<int64_t>(len) + from);
    for (uint64_t i = static_cast<uint64_t>(from); i < len; ++i) {
        TaggedValue el = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(i)) : GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(i));
        if (SameValue(el, search) || (el.IsNumber() && search.IsNumber() && std::isnan(el.AsNumber()) && std::isnan(search.AsNumber()))) {
            return TaggedValue::FromBoolean(true);
        }
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue Array_reverse(TaggedValue this_val, const std::vector<TaggedValue>&) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        std::reverse(arr->Elements().begin(), arr->Elements().end());
        return this_val;
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    uint64_t middle = len / 2;
    for (uint64_t lower = 0; lower < middle; ++lower) {
        uint64_t upper = len - lower - 1;
        std::string lower_s = std::to_string(lower);
        std::string upper_s = std::to_string(upper);
        bool lower_exists = HasProperty(TaggedValue::FromHeapObject(obj), lower_s);
        bool upper_exists = HasProperty(TaggedValue::FromHeapObject(obj), upper_s);
        TaggedValue lower_val = lower_exists ? GetProperty(TaggedValue::FromHeapObject(obj), lower_s) : TaggedValue::Undefined();
        TaggedValue upper_val = upper_exists ? GetProperty(TaggedValue::FromHeapObject(obj), upper_s) : TaggedValue::Undefined();
        if (lower_exists && upper_exists) {
            SetProperty(TaggedValue::FromHeapObject(obj), lower_s, upper_val);
            SetProperty(TaggedValue::FromHeapObject(obj), upper_s, lower_val);
        } else if (!lower_exists && upper_exists) {
            SetProperty(TaggedValue::FromHeapObject(obj), lower_s, upper_val);
            obj->DeleteNamedProperty(upper_s);
        } else if (lower_exists && !upper_exists) {
            obj->DeleteNamedProperty(lower_s);
            SetProperty(TaggedValue::FromHeapObject(obj), upper_s, lower_val);
        }
    }
    return this_val;
}

static TaggedValue Array_concat(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        for (size_t i = 0; i < arr->Length(); ++i) {
            new_arr->Push(arr->GetElement(i));
        }
    } else {
        new_arr->Push(TaggedValue::FromHeapObject(obj));
    }
    for (const auto& item : args) {
        if (item.IsHeapObject() && item.AsHeapObject()->IsArray()) {
            auto* other = static_cast<JSArray*>(item.AsHeapObject());
            for (size_t j = 0; j < other->Length(); ++j) {
                new_arr->Push(other->GetElement(j));
            }
        } else {
            new_arr->Push(item);
        }
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_map(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.map callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, static_cast<size_t>(len));
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val;
        bool present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                present = true;
            }
        }
        if (present) {
            TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            new_arr->SetElement(static_cast<size_t>(k), res);
        }
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_filter(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.filter callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val;
        bool present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                present = true;
            }
        }
        if (present) {
            TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            if (res.ToBoolean()) {
                new_arr->Push(k_val);
            }
        }
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_forEach(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.forEach callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val;
        bool present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                present = true;
            }
        }
        if (present) {
            CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        }
    }
    return TaggedValue::Undefined();
}

static TaggedValue Array_reduce(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.reduce callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    uint64_t k = 0;
    TaggedValue acc;
    if (args.size() > 1) {
        acc = args[1];
    } else {
        bool k_present = false;
        while (k < len) {
            if (obj->IsArray()) {
                auto* arr = static_cast<JSArray*>(obj);
                if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                    acc = arr->Elements()[k];
                    k_present = true;
                    k++;
                    break;
                }
            } else {
                std::string k_str = std::to_string(k);
                if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                    acc = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                    k_present = true;
                    k++;
                    break;
                }
            }
            k++;
        }
        if (!k_present) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Reduce of empty array with no initial value")));
            }
            return TaggedValue::Undefined();
        }
    }
    while (k < len) {
        TaggedValue k_val;
        bool k_present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                k_present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                k_present = true;
            }
        }
        if (k_present) {
            acc = CallFunction(cb, TaggedValue::Undefined(), {acc, k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                return TaggedValue::Undefined();
            }
        }
        k++;
    }
    return acc;
}

static TaggedValue Array_reduceRight(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.reduceRight callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    if (len == 0 && args.size() <= 1) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Reduce of empty array with no initial value")));
        }
        return TaggedValue::Undefined();
    }
    int64_t k = static_cast<int64_t>(len) - 1;
    TaggedValue acc;
    if (args.size() > 1) {
        acc = args[1];
    } else {
        bool k_present = false;
        while (k >= 0) {
            if (obj->IsArray()) {
                auto* arr = static_cast<JSArray*>(obj);
                if (static_cast<size_t>(k) < arr->Length() && static_cast<size_t>(k) < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                    acc = arr->Elements()[k];
                    k_present = true;
                    k--;
                    break;
                }
            } else {
                std::string k_str = std::to_string(k);
                if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                    acc = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                    k_present = true;
                    k--;
                    break;
                }
            }
            k--;
        }
        if (!k_present) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Reduce of empty array with no initial value")));
            }
            return TaggedValue::Undefined();
        }
    }
    while (k >= 0) {
        TaggedValue k_val;
        bool k_present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (static_cast<size_t>(k) < arr->Length() && static_cast<size_t>(k) < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                k_present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                k_present = true;
            }
        }
        if (k_present) {
            acc = CallFunction(cb, TaggedValue::Undefined(), {acc, k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                return TaggedValue::Undefined();
            }
        }
        k--;
    }
    return acc;
}

static TaggedValue Array_some(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.some callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val;
        bool k_present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                k_present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                k_present = true;
            }
        }
        if (k_present) {
            TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            if (res.ToBoolean()) return TaggedValue::FromBoolean(true);
        }
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue Array_every(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.every callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val;
        bool k_present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                k_val = arr->Elements()[k];
                k_present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(obj), k_str)) {
                k_val = GetProperty(TaggedValue::FromHeapObject(obj), k_str);
                k_present = true;
            }
        }
        if (k_present) {
            TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            if (!res.ToBoolean()) return TaggedValue::FromBoolean(false);
        }
    }
    return TaggedValue::FromBoolean(true);
}

static TaggedValue Array_find(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.find callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(k)) : GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(k));
        TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) return k_val;
    }
    return TaggedValue::Undefined();
}

static TaggedValue Array_findIndex(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.findIndex callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue t = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue k_val = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(k)) : GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(k));
        TaggedValue res = CallFunction(cb, t, {k_val, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(obj)});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) return k <= 0x7FFFFFFF ? TaggedValue::FromSmi(static_cast<int32_t>(k)) : TaggedValue::FromDouble(static_cast<double>(k));
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue Array_fill(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    int64_t relative_start = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].ToDouble())) : 0;
    int64_t k = relative_start < 0 ? std::max<int64_t>(static_cast<int64_t>(len) + relative_start, 0) : std::min<int64_t>(relative_start, static_cast<int64_t>(len));
    int64_t relative_end = args.size() > 2 && !args[2].IsUndefined() ? (args[2].IsSmi() ? args[2].AsSmi() : static_cast<int64_t>(args[2].ToDouble())) : static_cast<int64_t>(len);
    int64_t final_end = relative_end < 0 ? std::max<int64_t>(static_cast<int64_t>(len) + relative_end, 0) : std::min<int64_t>(relative_end, static_cast<int64_t>(len));
    while (k < final_end) {
        if (obj->IsArray()) {
            static_cast<JSArray*>(obj)->SetElement(static_cast<size_t>(k), val);
        } else {
            SetProperty(TaggedValue::FromHeapObject(obj), std::to_string(k), val);
        }
        k++;
    }
    return TaggedValue::FromHeapObject(obj);
}

static TaggedValue Array_copyWithin(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    int64_t rel_target = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble())) : 0;
    int64_t to = rel_target < 0 ? std::max<int64_t>(static_cast<int64_t>(len) + rel_target, 0) : std::min<int64_t>(rel_target, static_cast<int64_t>(len));
    int64_t rel_start = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].ToDouble())) : 0;
    int64_t from = rel_start < 0 ? std::max<int64_t>(static_cast<int64_t>(len) + rel_start, 0) : std::min<int64_t>(rel_start, static_cast<int64_t>(len));
    int64_t rel_end = args.size() > 2 && !args[2].IsUndefined() ? (args[2].IsSmi() ? args[2].AsSmi() : static_cast<int64_t>(args[2].ToDouble())) : static_cast<int64_t>(len);
    int64_t final_end = rel_end < 0 ? std::max<int64_t>(static_cast<int64_t>(len) + rel_end, 0) : std::min<int64_t>(rel_end, static_cast<int64_t>(len));
    int64_t count = std::min<int64_t>(final_end - from, static_cast<int64_t>(len) - to);
    int64_t direction = 1;
    if (from < to && to < from + count) {
        direction = -1;
        from = from + count - 1;
        to = to + count - 1;
    }
    while (count > 0) {
        TaggedValue from_val;
        bool from_present = false;
        if (obj->IsArray()) {
            auto* arr = static_cast<JSArray*>(obj);
            if (static_cast<size_t>(from) < arr->Length() && static_cast<size_t>(from) < arr->Elements().size() && !arr->Elements()[from].IsEmpty()) {
                from_val = arr->Elements()[from];
                from_present = true;
            }
        } else {
            std::string f_str = std::to_string(from);
            if (HasProperty(TaggedValue::FromHeapObject(obj), f_str)) {
                from_val = GetProperty(TaggedValue::FromHeapObject(obj), f_str);
                from_present = true;
            }
        }
        if (from_present) {
            if (obj->IsArray()) {
                static_cast<JSArray*>(obj)->SetElement(static_cast<size_t>(to), from_val);
            } else {
                SetProperty(TaggedValue::FromHeapObject(obj), std::to_string(to), from_val);
            }
        } else {
            if (obj->IsArray()) {
                static_cast<JSArray*>(obj)->SetElement(static_cast<size_t>(to), TaggedValue::Empty());
            } else {
                obj->DeleteNamedProperty(std::to_string(to));
            }
        }
        from += direction;
        to += direction;
        count--;
    }
    return TaggedValue::FromHeapObject(obj);
}

static TaggedValue Array_at(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    int64_t relative_idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble()));
    int64_t k = relative_idx >= 0 ? relative_idx : static_cast<int64_t>(len) + relative_idx;
    if (k < 0 || static_cast<uint64_t>(k) >= len) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        return static_cast<JSArray*>(obj)->GetElement(static_cast<size_t>(k));
    }
    return GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(k));
}

static void FlattenIntoArray(JSArray* target, JSObject* source, uint64_t source_len, int64_t depth, TaggedValue mapper = TaggedValue::Undefined(), TaggedValue this_arg = TaggedValue::Undefined()) {
    for (uint64_t k = 0; k < source_len; ++k) {
        TaggedValue element;
        bool present = false;
        if (source->IsArray()) {
            auto* arr = static_cast<JSArray*>(source);
            if (k < arr->Length() && k < arr->Elements().size() && !arr->Elements()[k].IsEmpty()) {
                element = arr->Elements()[k];
                present = true;
            }
        } else {
            std::string k_str = std::to_string(k);
            if (HasProperty(TaggedValue::FromHeapObject(source), k_str)) {
                element = GetProperty(TaggedValue::FromHeapObject(source), k_str);
                present = true;
            }
        }
        if (!present) continue;
        if (mapper.IsHeapObject() && mapper.AsHeapObject()->IsFunction()) {
            element = CallFunction(mapper, this_arg, {element, TaggedValue::FromDouble(static_cast<double>(k)), TaggedValue::FromHeapObject(source)});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return;
        }
        if (depth > 0 && element.IsHeapObject() && element.AsHeapObject()->IsArray()) {
            auto* child_arr = static_cast<JSArray*>(element.AsHeapObject());
            FlattenIntoArray(target, child_arr, child_arr->Length(), depth - 1);
        } else {
            target->Push(element);
        }
    }
}

static TaggedValue Array_flat(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    int64_t depth = 1;
    if (!args.empty() && !args[0].IsUndefined()) {
        double d = args[0].ToDouble();
        if (std::isnan(d) || d < 0) depth = 0;
        else if (d > 1000000.0) depth = 1000000;
        else depth = static_cast<int64_t>(d);
    }
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    FlattenIntoArray(new_arr, obj, len, depth);
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_flatMap(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.prototype.flatMap callback must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    uint64_t len = obj->IsArray() ? static_cast<JSArray*>(obj)->Length() : ToLength(obj->GetNamedProperty("length"));
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    FlattenIntoArray(new_arr, obj, len, 1, cb, this_arg);
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_of(const std::vector<TaggedValue>& args) {
    auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, args.size());
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (const auto& a : args) arr->Push(a);
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue Array_from(const std::vector<TaggedValue>& args) {
    if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.from requires an array-like or iterable object")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue items = args[0];
    TaggedValue map_fn = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    TaggedValue this_arg = args.size() > 2 ? args[2] : TaggedValue::Undefined();
    bool mapping = !map_fn.IsUndefined();
    if (mapping && (!map_fn.IsHeapObject() || !map_fn.AsHeapObject()->IsFunction())) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Array.from: when provided, the second argument must be a function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue iter_fn = GetProperty(items, "@@iterator");
    if (iter_fn.IsHeapObject() && iter_fn.AsHeapObject()->IsFunction()) {
        TaggedValue iter = CallFunction(iter_fn, items, {});
        auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        uint64_t k = 0;
        while (true) {
            TaggedValue next_res = CallFunction(GetProperty(iter, "next"), iter, {});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            if (GetProperty(next_res, "done").ToBoolean()) break;
            TaggedValue val = GetProperty(next_res, "value");
            if (mapping) {
                val = CallFunction(map_fn, this_arg, {val, TaggedValue::FromDouble(static_cast<double>(k))});
                if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            }
            arr->Push(val);
            k++;
        }
        return TaggedValue::FromHeapObject(arr);
    }
    JSObject* obj = ToObject(items);
    if (!obj) return TaggedValue::Undefined();
    uint64_t len = ToLength(GetProperty(items, "length"));
    auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, static_cast<size_t>(len));
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (uint64_t k = 0; k < len; ++k) {
        TaggedValue val = GetProperty(items, std::to_string(k));
        if (mapping) {
            val = CallFunction(map_fn, this_arg, {val, TaggedValue::FromDouble(static_cast<double>(k))});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        }
        arr->Push(val);
    }
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue Array_splice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        int64_t len = static_cast<int64_t>(arr->Length());
        if (args.empty()) {
            auto* empty_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, 0);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                empty_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            return TaggedValue::FromHeapObject(empty_arr);
        }
        int64_t start = args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble());
        if (start < 0) start = std::max<int64_t>(0, len + start);
        else start = std::min<int64_t>(len, start);

        int64_t delete_count = 0;
        if (args.size() == 1) {
            delete_count = len - start;
        } else {
            int64_t dc = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].ToDouble());
            delete_count = std::clamp<int64_t>(dc, 0, len - start);
        }

        std::vector<TaggedValue> removed;
        removed.reserve(static_cast<size_t>(delete_count));
        for (int64_t i = 0; i < delete_count; ++i) {
            removed.push_back(arr->GetElement(static_cast<size_t>(start + i)));
        }

        std::vector<TaggedValue>& elems = arr->Elements();
        std::vector<TaggedValue> items_to_insert;
        if (args.size() > 2) {
            items_to_insert.assign(args.begin() + 2, args.end());
        }

        auto it = elems.begin() + start;
        elems.erase(it, it + delete_count);
        elems.insert(elems.begin() + start, items_to_insert.begin(), items_to_insert.end());
        if (arr->IsOld()) {
            for (const auto& item : items_to_insert) {
                if (item.IsHeapObject()) RecordWriteBarrier(arr, item);
            }
        }
        arr->SetLength(elems.size());

        auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, std::move(removed));
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        return TaggedValue::FromHeapObject(new_arr);
    }
    uint64_t len = ToLength(obj->GetNamedProperty("length"));
    int64_t start = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble()));
    if (start < 0) start = std::max<int64_t>(0, static_cast<int64_t>(len) + start);
    else start = std::min<int64_t>(static_cast<int64_t>(len), start);
    int64_t delete_count = 0;
    if (args.size() == 1) delete_count = static_cast<int64_t>(len) - start;
    else if (args.size() > 1) {
        int64_t dc = args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].ToDouble());
        delete_count = std::clamp<int64_t>(dc, 0, static_cast<int64_t>(len) - start);
    }
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (int64_t i = 0; i < delete_count; ++i) {
        new_arr->Push(GetProperty(TaggedValue::FromHeapObject(obj), std::to_string(start + i)));
    }
    int64_t insert_count = args.size() > 2 ? static_cast<int64_t>(args.size() - 2) : 0;
    if (static_cast<int64_t>(len) + insert_count - delete_count > 9007199254740991LL) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Array splice length exceeds 2^53 - 1")));
        return TaggedValue::Undefined();
    }
    if (insert_count < delete_count) {
        for (int64_t i = start; i < static_cast<int64_t>(len) - delete_count; ++i) {
            std::string from = std::to_string(i + delete_count);
            std::string to = std::to_string(i + insert_count);
            if (HasProperty(TaggedValue::FromHeapObject(obj), from)) {
                SetProperty(TaggedValue::FromHeapObject(obj), to, GetProperty(TaggedValue::FromHeapObject(obj), from));
            } else {
                obj->DeleteNamedProperty(to);
            }
        }
        for (int64_t i = static_cast<int64_t>(len); i > static_cast<int64_t>(len) - delete_count + insert_count; --i) {
            obj->DeleteNamedProperty(std::to_string(i - 1));
        }
    } else if (insert_count > delete_count) {
        for (int64_t i = static_cast<int64_t>(len) - delete_count; i > start; --i) {
            std::string from = std::to_string(i + delete_count - 1);
            std::string to = std::to_string(i + insert_count - 1);
            if (HasProperty(TaggedValue::FromHeapObject(obj), from)) {
                SetProperty(TaggedValue::FromHeapObject(obj), to, GetProperty(TaggedValue::FromHeapObject(obj), from));
            } else {
                obj->DeleteNamedProperty(to);
            }
        }
    }
    for (int64_t i = 0; i < insert_count; ++i) {
        SetProperty(TaggedValue::FromHeapObject(obj), std::to_string(start + i), args[2 + i]);
    }
    uint64_t new_len = static_cast<uint64_t>(static_cast<int64_t>(len) - delete_count + insert_count);
    uint32_t sid = 0;
    obj->SetNamedProperty("length", TaggedValue::FromDouble(static_cast<double>(new_len)), sid);
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_sort(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    JSObject* obj = ToObject(this_val);
    if (!obj) return TaggedValue::Undefined();
    TaggedValue compare_fn = args.empty() ? TaggedValue::Undefined() : args[0];
    if (!compare_fn.IsUndefined() && (!compare_fn.IsHeapObject() || !compare_fn.AsHeapObject()->IsFunction())) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "The comparison function must be either a function or undefined")));
        }
        return TaggedValue::Undefined();
    }
    if (obj->IsArray()) {
        auto* arr = static_cast<JSArray*>(obj);
        std::vector<TaggedValue>& elems = arr->Elements();
        if (compare_fn.IsHeapObject() && compare_fn.AsHeapObject()->IsFunction()) {
            std::stable_sort(elems.begin(), elems.end(), [&](const TaggedValue& a, const TaggedValue& b) {
                if (a.IsUndefined() && b.IsUndefined()) return false;
                if (a.IsUndefined()) return false;
                if (b.IsUndefined()) return true;
                std::vector<TaggedValue> cmp_args = {a, b};
                TaggedValue res = CallFunction(compare_fn, TaggedValue::Undefined(), cmp_args);
                double d = res.ToDouble();
                return d < 0;
            });
        } else {
            std::stable_sort(elems.begin(), elems.end(), [](const TaggedValue& a, const TaggedValue& b) {
                if (a.IsUndefined() && b.IsUndefined()) return false;
                if (a.IsUndefined()) return false;
                if (b.IsUndefined()) return true;
                return a.ToString() < b.ToString();
            });
        }
    }
    return this_val;
}

static TaggedValue Array_isArray(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(args[0].IsHeapObject() && args[0].AsHeapObject()->IsArray());
}

// String Prototype Methods
static std::string GetStringFromThis(TaggedValue this_val) {
    if (this_val.IsHeapObject()) {
        HeapObject* ho = this_val.AsHeapObject();
        if (ho->IsString()) {
            return static_cast<JSString*>(ho)->Value();
        }
        if (ho->IsJSObject()) {
            auto* jso = static_cast<JSObject*>(ho);
            if (jso->HasNamedProperty("__string_data__")) {
                return jso->GetNamedProperty("__string_data__").ToString();
            }
        }
    }
    return this_val.ToString();
}

static TaggedValue String_charAt(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    int32_t idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int32_t>(args[0].AsNumber()));
    if (idx >= 0 && static_cast<size_t>(idx) < s.length()) {
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(idx, 1)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
}

static TaggedValue String_charCodeAt(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    int32_t idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int32_t>(args[0].AsNumber()));
    if (idx >= 0 && static_cast<size_t>(idx) < s.length()) {
        return TaggedValue::FromSmi(static_cast<uint8_t>(s[idx]));
    }
    return TaggedValue::FromDouble(std::nan(""));
}

static TaggedValue String_concat(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string str = this_val.ToString();
    for (const auto& a : args) {
        str += a.ToString();
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str));
}

static TaggedValue String_indexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    size_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<size_t>(std::max<int32_t>(0, args[1].AsSmi())) : 0;
    size_t pos = s.find(search, from);
    return TaggedValue::FromSmi(pos != std::string::npos ? static_cast<int32_t>(pos) : -1);
}

static TaggedValue String_lastIndexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    size_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<size_t>(std::max<int32_t>(0, args[1].AsSmi())) : s.length();
    if (from > s.length()) from = s.length();
    size_t pos = s.rfind(search, from);
    return TaggedValue::FromSmi(pos != std::string::npos ? static_cast<int32_t>(pos) : -1);
}

static TaggedValue String_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    int64_t len = static_cast<int64_t>(s.length());
    int64_t start = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;

    if (start < 0) start = std::max<int64_t>(0, len + start);
    else start = std::min<int64_t>(len, start);

    if (end < 0) end = std::max<int64_t>(0, len + end);
    else end = std::min<int64_t>(len, end);

    if (start >= end) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(start, end - start)));
}

static TaggedValue String_substring(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s = GetStringFromThis(this_val);
    int64_t len = static_cast<int64_t>(s.length());
    int64_t start = args.size() > 0 ? std::max<int64_t>(0, args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? std::max<int64_t>(0, args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;

    start = std::min<int64_t>(len, start);
    end = std::min<int64_t>(len, end);
    if (start > end) std::swap(start, end);

    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(start, end - start)));
}

static TaggedValue String_toLowerCase(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string s = GetStringFromThis(this_val);
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
}

static TaggedValue String_toUpperCase(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string s = GetStringFromThis(this_val);
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
}

static TaggedValue String_trim(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string s = GetStringFromThis(this_val);
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(first, last - first)));
}

static TaggedValue String_split(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string s_str = GetStringFromThis(this_val);
    auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    if (args.empty()) {
        arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s_str)));
        return TaggedValue::FromHeapObject(arr);
    }

    if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsRegExp()) {
        auto* re = static_cast<JSRegExp*>(args[0].AsHeapObject());
        try {
            RegexBudget budget;
            BoundedRegex regex = BoundedRegex::Compile(re->Pattern(), re->Flags(), budget);
            size_t cursor = 0;
            size_t search = 0;
            while (search <= s_str.size()) {
                auto match = regex.Search(s_str, search, budget);
                if (!match) break;
                const auto full = match->Full();
                arr->Push(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSString>(s_str.substr(cursor, full.begin - cursor))));
                for (size_t i = 1; i < match->captures.size(); ++i) {
                    if (match->captures[i].has_value()) {
                        const auto capture = *match->captures[i];
                        arr->Push(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSString>(s_str.substr(capture.begin, capture.end - capture.begin))));
                    }
                }
                cursor = full.end;
                auto next = NextRegexSearchOffset(*match, s_str.size());
                if (!next) break;
                search = *next;
            }
            arr->Push(TaggedValue::FromHeapObject(
                AllocateRuntime<JSString>(s_str.substr(cursor))));
        } catch (const RegexError& error) {
            SetRegexException(error);
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromHeapObject(arr);
    }

    std::string sep = args[0].ToString();
    if (sep.empty()) {
        for (char c : s_str) {
            arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(1, c))));
        }
        return TaggedValue::FromHeapObject(arr);
    }

    size_t pos = 0;
    while ((pos = s_str.find(sep)) != std::string::npos) {
        arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s_str.substr(0, pos))));
        s_str.erase(0, pos + sep.length());
    }
    arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s_str)));
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue String_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromBoolean(false);
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    return TaggedValue::FromBoolean(s->View().find(search) != std::string_view::npos);
}

static TaggedValue String_replace(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string str = this_val.ToString();
    if (args.empty()) {
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str));
    }
    std::string replacement = args.size() > 1 ? args[1].ToString() : "undefined";

    if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsRegExp()) {
        auto* re = static_cast<JSRegExp*>(args[0].AsHeapObject());
        bool is_global = re->Flags().find('g') != std::string::npos;
        try {
            RegexBudget budget;
            BoundedRegex regex = BoundedRegex::Compile(re->Pattern(), re->Flags(), budget);
            std::string res;
            size_t cursor = 0;
            size_t search = 0;
            bool replaced = false;
            while (search <= str.size()) {
                auto match = regex.Search(str, search, budget);
                if (!match) break;
                const auto full = match->Full();
                res.append(str.substr(cursor, full.begin - cursor));
                AppendRegexReplacement(res, replacement, str, *match);
                cursor = full.end;
                replaced = true;
                if (!is_global) break;
                auto next = NextRegexSearchOffset(*match, str.size());
                if (!next) break;
                search = *next;
            }
            if (!replaced) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str));
            res.append(str.substr(cursor));
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
        } catch (const RegexError& error) {
            SetRegexException(error);
            return TaggedValue::Undefined();
        }
    }

    std::string pattern = args[0].ToString();
    size_t pos = str.find(pattern);
    if (pos != std::string::npos) {
        str.replace(pos, pattern.length(), replacement);
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str));
}

static TaggedValue String_repeat(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    std::string str = this_val.ToString();
    if (args.empty()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    const double count_value = args[0].IsSmi() ? static_cast<double>(args[0].AsSmi()) : args[0].ToDouble();
    if (!std::isfinite(count_value) || count_value < 0.0 || std::floor(count_value) != count_value) {
        SetRangeException("Invalid count value");
        return TaggedValue::Undefined();
    }
    if (!str.empty() && count_value > static_cast<double>(kMaxStringLength / str.size())) {
        SetRangeException("String length exceeds maximum allowed length");
        return TaggedValue::Undefined();
    }
    const size_t count = static_cast<size_t>(count_value);
    std::string res;
    res.reserve(str.length() * count);
    for (size_t i = 0; i < count; ++i) {
        res += str;
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

static TaggedValue String_startsWith(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsRegExp()) {
        SetTypeException("First argument to String.prototype.startsWith must not be a regular expression");
        return TaggedValue::Undefined();
    }
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    int64_t pos = 0;
    if (args.size() > 1) {
        double p = args[1].ToDouble();
        if (std::isnan(p) || p <= 0.0) pos = 0;
        else pos = static_cast<int64_t>(p);
    }
    int64_t len = static_cast<int64_t>(s.length());
    int64_t start = std::min<int64_t>(std::max<int64_t>(0, pos), len);
    int64_t search_len = static_cast<int64_t>(search.length());
    if (start + search_len > len) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(s.compare(static_cast<size_t>(start), static_cast<size_t>(search_len), search) == 0);
}

static TaggedValue String_endsWith(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsRegExp()) {
        SetTypeException("First argument to String.prototype.endsWith must not be a regular expression");
        return TaggedValue::Undefined();
    }
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    int64_t len = static_cast<int64_t>(s.length());
    int64_t end = len;
    if (args.size() > 1 && !args[1].IsUndefined()) {
        double p = args[1].ToDouble();
        if (std::isnan(p) || p <= 0.0) end = 0;
        else end = std::min<int64_t>(len, static_cast<int64_t>(p));
    }
    int64_t search_len = static_cast<int64_t>(search.length());
    if (search_len == 0) return TaggedValue::FromBoolean(true);
    if (end < search_len) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(s.compare(static_cast<size_t>(end - search_len), static_cast<size_t>(search_len), search) == 0);
}

static TaggedValue String_padStart(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    if (args.empty()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    int64_t max_len = static_cast<int64_t>(ToLength(args[0]));
    int64_t str_len = static_cast<int64_t>(s.length());
    if (max_len <= str_len) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    std::string filler = " ";
    if (args.size() > 1 && !args[1].IsUndefined()) {
        filler = args[1].ToString();
        if (filler.empty()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    }
    int64_t fill_len = max_len - str_len;
    std::string pad;
    pad.reserve(fill_len);
    while (static_cast<int64_t>(pad.length()) < fill_len) {
        pad += filler;
    }
    pad.resize(static_cast<size_t>(fill_len));
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(pad + s));
}

static TaggedValue String_padEnd(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    if (args.empty()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    int64_t max_len = static_cast<int64_t>(ToLength(args[0]));
    int64_t str_len = static_cast<int64_t>(s.length());
    if (max_len <= str_len) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    std::string filler = " ";
    if (args.size() > 1 && !args[1].IsUndefined()) {
        filler = args[1].ToString();
        if (filler.empty()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
    }
    int64_t fill_len = max_len - str_len;
    std::string pad;
    pad.reserve(fill_len);
    while (static_cast<int64_t>(pad.length()) < fill_len) {
        pad += filler;
    }
    pad.resize(static_cast<size_t>(fill_len));
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s + pad));
}

static TaggedValue String_codePointAt(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    int64_t pos = 0;
    if (!args.empty()) {
        double d = args[0].ToDouble();
        if (std::isnan(d) || d < 0.0) return TaggedValue::Undefined();
        pos = static_cast<int64_t>(d);
    }
    int64_t size = static_cast<int64_t>(s.size());
    if (pos < 0 || pos >= size) return TaggedValue::Undefined();

    uint8_t c0 = static_cast<uint8_t>(s[pos]);
    if (c0 < 0x80) return TaggedValue::FromSmi(c0);
    if ((c0 & 0xE0) == 0xC0 && pos + 1 < size) {
        uint32_t cp = ((c0 & 0x1F) << 6) | (static_cast<uint8_t>(s[pos + 1]) & 0x3F);
        return TaggedValue::FromSmi(static_cast<int32_t>(cp));
    }
    if ((c0 & 0xF0) == 0xE0 && pos + 2 < size) {
        uint32_t cp = ((c0 & 0x0F) << 12) | ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 6) | (static_cast<uint8_t>(s[pos + 2]) & 0x3F);
        return TaggedValue::FromSmi(static_cast<int32_t>(cp));
    }
    if ((c0 & 0xF8) == 0xF0 && pos + 3 < size) {
        uint32_t cp = ((c0 & 0x07) << 18) | ((static_cast<uint8_t>(s[pos + 1]) & 0x3F) << 12) | ((static_cast<uint8_t>(s[pos + 2]) & 0x3F) << 6) | (static_cast<uint8_t>(s[pos + 3]) & 0x3F);
        return TaggedValue::FromSmi(static_cast<int32_t>(cp));
    }
    return TaggedValue::FromSmi(c0);
}

static TaggedValue String_at(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (this_val.IsUndefined() || this_val.IsNull()) {
        SetTypeException("Cannot convert undefined or null to object");
        return TaggedValue::Undefined();
    }
    std::string s = this_val.ToString();
    int64_t len = static_cast<int64_t>(s.length());
    int64_t rel_idx = 0;
    if (!args.empty()) {
        double d = args[0].ToDouble();
        if (std::isnan(d)) rel_idx = 0;
        else rel_idx = static_cast<int64_t>(d);
    }
    int64_t k = (rel_idx >= 0) ? rel_idx : (len + rel_idx);
    if (k < 0 || k >= len) return TaggedValue::Undefined();
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(static_cast<size_t>(k), 1)));
}

static TaggedValue Function_call(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function.prototype.call called on non-function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue target_this = args.empty() ? TaggedValue::Undefined() : args[0];
    std::vector<TaggedValue> call_args;
    if (args.size() > 1) {
        call_args.assign(args.begin() + 1, args.end());
    }
    return CallFunction(this_val, target_this, call_args);
}

static TaggedValue Function_apply(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function.prototype.apply called on non-function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue target_this = args.empty() ? TaggedValue::Undefined() : args[0];
    std::vector<TaggedValue> call_args;
    if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsArray()) {
        auto* arr = static_cast<JSArray*>(args[1].AsHeapObject());
        for (size_t i = 0; i < arr->Length(); ++i) {
            call_args.push_back(arr->GetElement(i));
        }
    }
    return CallFunction(this_val, target_this, call_args);
}

static TaggedValue Function_bind(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsFunction()) {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Function.prototype.bind called on non-function")));
        }
        return TaggedValue::Undefined();
    }
    TaggedValue bound_this = args.empty() ? TaggedValue::Undefined() : args[0];
    std::vector<TaggedValue> bound_args;
    if (args.size() > 1) {
        bound_args.assign(args.begin() + 1, args.end());
    }
    auto* bound_fn = AllocateRuntime<JSNativeFunction>("bound", [this_val, bound_this, bound_args](const std::vector<TaggedValue>& extra_args) -> TaggedValue {
        std::vector<TaggedValue> full_args = bound_args;
        full_args.insert(full_args.end(), extra_args.begin(), extra_args.end());
        return CallFunction(this_val, bound_this, full_args);
    });
    uint32_t b_sid = 9500;
    bound_fn->SetNamedProperty("__bound_target__", this_val, b_sid);
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        bound_fn->SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
    return TaggedValue::FromHeapObject(bound_fn);
}

static TaggedValue Function_hasInstance(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    TaggedValue V = args.empty() ? TaggedValue::Undefined() : args[0];
    return OrdinaryHasInstance(this_val, V);
}

static TaggedValue Object_hasOwnProperty(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (args.empty() || !this_val.IsHeapObject()) return TaggedValue::FromBoolean(false);
    std::string key = ToPropertyKeyString(args[0]);
    auto* ho = this_val.AsHeapObject();
    if (ho->IsJSObject()) {
        return TaggedValue::FromBoolean(static_cast<JSObject*>(ho)->HasOwnProperty(key));
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue Object_propertyIsEnumerable(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (args.empty() || !this_val.IsHeapObject()) return TaggedValue::FromBoolean(false);
    std::string key = args[0].ToString();
    auto* ho = this_val.AsHeapObject();
    if (ho->IsJSObject()) {
        auto* obj = static_cast<JSObject*>(ho);
        bool has_own = false;
        if (obj->HasAccessor(key)) {
            has_own = true;
        } else {
            Shape* shape = obj->GetShape();
            if (shape != nullptr) {
                const PropertyDescriptor* desc = shape->LookupProperty(key);
                if (desc != nullptr) {
                    TaggedValue val = obj->GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) has_own = true;
                }
            }
            if (!has_own && obj->IsArray()) {
                if (key == "length") {
                    has_own = true;
                } else {
                    auto is_digits = [](std::string_view sv) {
                        if (sv.empty()) return false;
                        for (char c : sv) if (!std::isdigit(static_cast<unsigned char>(c))) return false;
                        return true;
                    };
                    if (is_digits(key)) {
                        size_t idx = static_cast<size_t>(std::stoull(key));
                        auto* arr = static_cast<const JSArray*>(obj);
                        if (idx < arr->Length() && !arr->Elements()[idx].IsEmpty()) {
                            has_own = true;
                        }
                    }
                }
            }
        }
        if (!has_own) return TaggedValue::FromBoolean(false);
        PropertyAttributes attrs = obj->GetOwnPropertyAttributes(key);
        return TaggedValue::FromBoolean(static_cast<uint8_t>(attrs) & static_cast<uint8_t>(PropertyAttributes::Enumerable));
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue Object_values(const std::vector<TaggedValue>& args) {
    if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
        return TaggedValue::Undefined();
    }
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* arr = AllocateRuntime<JSArray>(root);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    TaggedValue obj = args[0];
    if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) {
        auto* jso = static_cast<JSObject*>(obj.AsHeapObject());
        auto keys = jso->GetOwnPropertyNames();
        for (const auto& k : keys) {
            if (jso->IsArray() && k == "length") continue;
            arr->Push(GetProperty(obj, k));
        }
    }
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue Object_entries(const std::vector<TaggedValue>& args) {
    if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
        return TaggedValue::Undefined();
    }
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* arr = AllocateRuntime<JSArray>(root);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    TaggedValue obj = args[0];
    if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) {
        auto* jso = static_cast<JSObject*>(obj.AsHeapObject());
        auto keys = jso->GetOwnPropertyNames();
        for (const auto& k : keys) {
            if (jso->IsArray() && k == "length") continue;
            auto* pair = AllocateRuntime<JSArray>(root);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                pair->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            pair->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(k)));
            pair->Push(GetProperty(obj, k));
            arr->Push(TaggedValue::FromHeapObject(pair));
        }
    }
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue Object_fromEntries(const std::vector<TaggedValue>& args) {
    if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Object.fromEntries requires an iterable argument")));
        return TaggedValue::Undefined();
    }
    Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* obj = AllocateRuntime<JSObject>(root);
    TaggedValue iter = args[0];
    if (iter.IsHeapObject() && iter.AsHeapObject()->IsArray()) {
        auto* arr = static_cast<JSArray*>(iter.AsHeapObject());
        for (size_t i = 0; i < arr->Length(); ++i) {
            TaggedValue entry = arr->GetElement(i);
            if (entry.IsHeapObject() && entry.AsHeapObject()->IsArray()) {
                auto* entry_arr = static_cast<JSArray*>(entry.AsHeapObject());
                if (entry_arr->Length() >= 2) {
                    std::string key = entry_arr->GetElement(0).ToString();
                    TaggedValue val = entry_arr->GetElement(1);
                    uint32_t sid = 9500;
                    obj->SetNamedProperty(key, val, sid);
                }
            }
        }
    }
    return TaggedValue::FromHeapObject(obj);
}

static TaggedValue Object_is(const std::vector<TaggedValue>& args) {
    TaggedValue x = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    TaggedValue y = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    if (x.IsNumber() && y.IsNumber()) {
        double dx = x.AsNumber();
        double dy = y.AsNumber();
        if (std::isnan(dx) && std::isnan(dy)) return TaggedValue::FromBoolean(true);
        if (dx == 0.0 && dy == 0.0) {
            return TaggedValue::FromBoolean(std::signbit(dx) == std::signbit(dy));
        }
        return TaggedValue::FromBoolean(dx == dy);
    }
    return TaggedValue::FromBoolean(StrictEqualValues(x, y));
}

static TaggedValue Object_freeze(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::Undefined();
    TaggedValue val = args[0];
    if (val.IsHeapObject() && val.AsHeapObject()->IsJSObject()) {
        static_cast<JSObject*>(val.AsHeapObject())->Freeze();
    }
    return val;
}

static TaggedValue Object_isFrozen(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromBoolean(true);
    TaggedValue val = args[0];
    if (!val.IsHeapObject() || !val.AsHeapObject()->IsJSObject()) return TaggedValue::FromBoolean(true);
    return TaggedValue::FromBoolean(static_cast<JSObject*>(val.AsHeapObject())->IsFrozen());
}

static TaggedValue Object_seal(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::Undefined();
    TaggedValue val = args[0];
    if (val.IsHeapObject() && val.AsHeapObject()->IsJSObject()) {
        static_cast<JSObject*>(val.AsHeapObject())->Seal();
    }
    return val;
}

static TaggedValue Object_isSealed(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromBoolean(true);
    TaggedValue val = args[0];
    if (!val.IsHeapObject() || !val.AsHeapObject()->IsJSObject()) return TaggedValue::FromBoolean(true);
    return TaggedValue::FromBoolean(static_cast<JSObject*>(val.AsHeapObject())->IsSealed());
}

static TaggedValue Object_hasOwn(const std::vector<TaggedValue>& args) {
    if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
        if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
        return TaggedValue::Undefined();
    }
    std::vector<TaggedValue> has_args;
    if (args.size() > 1) has_args.push_back(args[1]);
    return Object_hasOwnProperty(args[0], has_args);
}

static TaggedValue Global_parseInt(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    std::string s = args[0].ToString();
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    s = s.substr(start);

    int radix = 10;
    if (args.size() > 1 && !args[1].IsUndefined()) {
        radix = static_cast<int>(args[1].ToDouble());
        if (radix == 0) radix = 10;
        else if (radix < 2 || radix > 36) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    }
    if ((radix == 10 || radix == 16) && s.size() >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        radix = 16;
        s = s.substr(2);
    }
    char* end = nullptr;
    long long val = std::strtoll(s.c_str(), &end, radix);
    if (end == s.c_str()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    if (val >= -1073741824LL && val <= 1073741823LL) {
        return TaggedValue::FromSmi(static_cast<int32_t>(val));
    }
    return TaggedValue::FromDouble(static_cast<double>(val));
}

static TaggedValue Global_parseFloat(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    std::string s = args[0].ToString();
    size_t start = s.find_first_not_of(" \t\n\r");
    if (start == std::string::npos) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    s = s.substr(start);

    char* end = nullptr;
    double val = std::strtod(s.c_str(), &end);
    if (end == s.c_str()) return TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN());
    return TaggedValue::FromDouble(val);
}

// JSON Parser & Serializer
static std::string StringifyValue(TaggedValue val) {
    if (val.IsUndefined()) return "undefined";
    if (val.IsNull()) return "null";
    if (val.IsTrue()) return "true";
    if (val.IsFalse()) return "false";
    if (val.IsSmi()) return std::to_string(val.AsSmi());
    if (val.IsDouble()) {
        std::ostringstream ss;
        ss << val.AsDouble();
        return ss.str();
    }
    if (val.IsHeapObject()) {
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsString()) {
            std::string s = static_cast<JSString*>(ho)->Value();
            std::string out = "\"";
            for (char c : s) {
                if (c == '"') out += "\\\"";
                else if (c == '\\') out += "\\\\";
                else if (c == '\n') out += "\\n";
                else if (c == '\r') out += "\\r";
                else if (c == '\t') out += "\\t";
                else out += c;
            }
            out += "\"";
            return out;
        }
        if (ho->IsArray()) {
            auto* arr = static_cast<JSArray*>(ho);
            std::string out = "[";
            for (size_t i = 0; i < arr->Length(); ++i) {
                if (i > 0) out += ",";
                TaggedValue el = arr->GetElement(i);
                if (el.IsUndefined()) out += "null";
                else out += StringifyValue(el);
            }
            out += "]";
            return out;
        }
        if (ho->IsPlainObject()) {
            auto* obj = static_cast<JSObject*>(ho);
            Shape* shape = obj->GetShape();
            std::string out = "{";
            bool first = true;
            if (shape) {
                for (const auto& [prop_name, desc] : shape->GetPropertyMap()) {
                    if (!first) out += ",";
                    first = false;
                    out += "\"" + prop_name + "\":" + StringifyValue(obj->GetNamedProperty(prop_name));
                }
            }
            out += "}";
            return out;
        }
    }
    return "null";
}

static TaggedValue JSON_stringify(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::Undefined();
    std::string json = StringifyValue(args[0]);
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(json));
}

class SimpleJSONParser {
public:
    explicit SimpleJSONParser(std::string_view text) : text_(text), pos_(0) {}

    TaggedValue Parse() {
        SkipWhitespace();
        if (pos_ >= text_.size()) return TaggedValue::Undefined();
        return ParseValue();
    }

private:
    void SkipWhitespace() {
        while (pos_ < text_.size() && (text_[pos_] == ' ' || text_[pos_] == '\t' || text_[pos_] == '\n' || text_[pos_] == '\r')) {
            pos_++;
        }
    }

    TaggedValue ParseValue() {
        SkipWhitespace();
        if (pos_ >= text_.size()) return TaggedValue::Undefined();
        char c = text_[pos_];
        if (c == 'n') return ParseNull();
        if (c == 't' || c == 'f') return ParseBool();
        if (c == '"') return ParseString();
        if (c == '[') return ParseArray();
        if (c == '{') return ParseObject();
        if (c == '-' || std::isdigit(static_cast<unsigned char>(c))) return ParseNumber();
        return TaggedValue::Undefined();
    }

    TaggedValue ParseNull() {
        if (text_.substr(pos_, 4) == "null") {
            pos_ += 4;
            return TaggedValue::Null();
        }
        return TaggedValue::Undefined();
    }

    TaggedValue ParseBool() {
        if (text_.substr(pos_, 4) == "true") {
            pos_ += 4;
            return TaggedValue::True();
        }
        if (text_.substr(pos_, 5) == "false") {
            pos_ += 5;
            return TaggedValue::False();
        }
        return TaggedValue::Undefined();
    }

    TaggedValue ParseNumber() {
        size_t start = pos_;
        if (text_[pos_] == '-') pos_++;
        while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_++;
        bool is_float = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            is_float = true;
            pos_++;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_++;
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            is_float = true;
            pos_++;
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) pos_++;
            while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_]))) pos_++;
        }
        std::string num_str(text_.substr(start, pos_ - start));
        if (is_float) {
            return TaggedValue::FromDouble(std::stod(num_str));
        }
        try {
            int64_t val = std::stoll(num_str);
            if (val >= INT32_MIN && val <= INT32_MAX) {
                return TaggedValue::FromSmi(static_cast<int32_t>(val));
            }
            return TaggedValue::FromDouble(static_cast<double>(val));
        } catch (...) {
            return TaggedValue::FromDouble(std::stod(num_str));
        }
    }

    TaggedValue ParseString() {
        pos_++; // skip initial '"'
        std::string res;
        while (pos_ < text_.size()) {
            char c = text_[pos_++];
            if (c == '"') {
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
            }
            if (c == '\\' && pos_ < text_.size()) {
                char esc = text_[pos_++];
                if (esc == '"') res += '"';
                else if (esc == '\\') res += '\\';
                else if (esc == '/') res += '/';
                else if (esc == 'n') res += '\n';
                else if (esc == 'r') res += '\r';
                else if (esc == 't') res += '\t';
                else res += esc;
            } else {
                res += c;
            }
        }
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
    }

    TaggedValue ParseArray() {
        pos_++; // skip '['
        auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == ']') {
            pos_++;
            return TaggedValue::FromHeapObject(arr);
        }
        while (pos_ < text_.size()) {
            TaggedValue val = ParseValue();
            arr->Push(val);
            SkipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                pos_++;
                SkipWhitespace();
            } else if (pos_ < text_.size() && text_[pos_] == ']') {
                pos_++;
                break;
            } else {
                break;
            }
        }
        return TaggedValue::FromHeapObject(arr);
    }

    TaggedValue ParseObject() {
        pos_++; // skip '{'
        auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        uint32_t next_shape_id = 1;
        SkipWhitespace();
        if (pos_ < text_.size() && text_[pos_] == '}') {
            pos_++;
            return TaggedValue::FromHeapObject(obj);
        }
        while (pos_ < text_.size()) {
            SkipWhitespace();
            if (text_[pos_] != '"') break;
            TaggedValue key_val = ParseString();
            std::string key = key_val.ToString();
            SkipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ':') {
                pos_++;
            }
            TaggedValue val = ParseValue();
            obj->SetNamedProperty(key, val, next_shape_id);
            SkipWhitespace();
            if (pos_ < text_.size() && text_[pos_] == ',') {
                pos_++;
                SkipWhitespace();
            } else if (pos_ < text_.size() && text_[pos_] == '}') {
                pos_++;
                break;
            } else {
                break;
            }
        }
        return TaggedValue::FromHeapObject(obj);
    }

    std::string_view text_;
    size_t pos_;
};

static TaggedValue JSON_parse(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::Undefined();
    std::string text = args[0].ToString();
    SimpleJSONParser parser(text);
    return parser.Parse();
}

static TaggedValue Object_toString(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (this_val.IsUndefined()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("[object Undefined]"));
    if (this_val.IsNull()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("[object Null]"));
    if (this_val.IsHeapObject()) {
        HeapObject* ho = this_val.AsHeapObject();
        if (ho->IsJSObject()) {
            TaggedValue tag = static_cast<JSObject*>(ho)->GetNamedProperty("@@toStringTag");
            if (tag.IsHeapObject() && tag.AsHeapObject()->IsString()) {
                std::string tag_str = std::string(static_cast<JSString*>(tag.AsHeapObject())->View());
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("[object " + tag_str + "]"));
            }
        }
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("[object Object]"));
}

static bool ExtractIterableElements(TaggedValue iterable_val, std::vector<TaggedValue>& out_items, TaggedValue& error_val) {
    if (!iterable_val.IsHeapObject()) {
        error_val = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Argument is not iterable"));
        return false;
    }
    HeapObject* ho = iterable_val.AsHeapObject();
    if (ho->IsArray()) {
        auto* arr = static_cast<JSArray*>(ho);
        size_t len = arr->Length();
        out_items.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            out_items.push_back(arr->GetElement(i));
        }
        return true;
    }
    TaggedValue iter_fn = GetProperty(iterable_val, "@@iterator");
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
        error_val = Interpreter::Current()->PendingException();
        Interpreter::Current()->ClearPendingException();
        return false;
    }
    if (iter_fn.IsHeapObject() && (iter_fn.AsHeapObject()->IsFunction() || iter_fn.AsHeapObject()->IsNativeFunction())) {
        TaggedValue iter = CallFunction(iter_fn, iterable_val, {});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            error_val = Interpreter::Current()->PendingException();
            Interpreter::Current()->ClearPendingException();
            return false;
        }
        TaggedValue next_fn = GetProperty(iter, "next");
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            error_val = Interpreter::Current()->PendingException();
            Interpreter::Current()->ClearPendingException();
            return false;
        }
        while (true) {
            TaggedValue next_res = CallFunction(next_fn, iter, {});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
                error_val = Interpreter::Current()->PendingException();
                Interpreter::Current()->ClearPendingException();
                return false;
            }
            if (GetProperty(next_res, "done").ToBoolean()) break;
            out_items.push_back(GetProperty(next_res, "value"));
        }
        return true;
    }
    error_val = TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Argument is not iterable"));
    return false;
}

// Map Intrinsics
static TaggedValue Map_ctor(const std::vector<TaggedValue>& args) {
    auto* m = AllocateRuntime<JSMap>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetMapPrototype()) {
        m->SetPrototype(Realm::Current()->GetMapPrototype());
    }
    if (!args.empty() && !args[0].IsNull() && !args[0].IsUndefined()) {
        std::vector<TaggedValue> items;
        TaggedValue err = TaggedValue::Undefined();
        if (ExtractIterableElements(args[0], items, err)) {
            for (const auto& entry : items) {
                TaggedValue k = TaggedValue::Undefined();
                TaggedValue v = TaggedValue::Undefined();
                if (entry.IsHeapObject() && entry.AsHeapObject()->IsArray()) {
                    auto* arr = static_cast<JSArray*>(entry.AsHeapObject());
                    if (arr->Length() > 0) k = arr->GetElement(0);
                    if (arr->Length() > 1) v = arr->GetElement(1);
                } else if (entry.IsHeapObject() && entry.AsHeapObject()->IsJSObject()) {
                    k = GetProperty(entry, "0");
                    v = GetProperty(entry, "1");
                } else {
                    SetTypeException("Iterator value is not an entry object");
                    return TaggedValue::Undefined();
                }
                m->Set(k, v);
            }
        } else if (Interpreter::Current() && !err.IsUndefined()) {
            Interpreter::Current()->SetPendingException(err);
            return TaggedValue::Undefined();
        }
    }
    return TaggedValue::FromHeapObject(m);
}

static TaggedValue Map_set(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsMap()) return this_val;
    auto* m = static_cast<JSMap*>(this_val.AsHeapObject());
    TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    TaggedValue v = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    m->Set(k, v);
    return this_val;
}

static TaggedValue Map_get(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsMap()) return TaggedValue::Undefined();
    auto* m = static_cast<JSMap*>(this_val.AsHeapObject());
    TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    return m->Get(k);
}

static TaggedValue Map_has(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsMap()) return TaggedValue::FromBoolean(false);
    auto* m = static_cast<JSMap*>(this_val.AsHeapObject());
    TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    return TaggedValue::FromBoolean(m->Has(k));
}

static TaggedValue Map_delete(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsMap()) return TaggedValue::FromBoolean(false);
    auto* m = static_cast<JSMap*>(this_val.AsHeapObject());
    TaggedValue k = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    return TaggedValue::FromBoolean(m->Delete(k));
}

static TaggedValue Map_clear(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsMap()) return TaggedValue::Undefined();
    auto* m = static_cast<JSMap*>(this_val.AsHeapObject());
    m->Clear();
    return TaggedValue::Undefined();
}

// Set Intrinsics
static TaggedValue Set_ctor(const std::vector<TaggedValue>& args) {
    auto* s = AllocateRuntime<JSSet>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetSetPrototype()) {
        s->SetPrototype(Realm::Current()->GetSetPrototype());
    }
    if (!args.empty() && !args[0].IsNull() && !args[0].IsUndefined()) {
        std::vector<TaggedValue> items;
        TaggedValue err = TaggedValue::Undefined();
        if (ExtractIterableElements(args[0], items, err)) {
            for (const auto& item : items) {
                s->Add(item);
            }
        } else if (Interpreter::Current() && !err.IsUndefined()) {
            Interpreter::Current()->SetPendingException(err);
            return TaggedValue::Undefined();
        }
    }
    return TaggedValue::FromHeapObject(s);
}

static TaggedValue Set_add(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsSet()) return this_val;
    auto* s = static_cast<JSSet*>(this_val.AsHeapObject());
    TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    s->Add(v);
    return this_val;
}

static TaggedValue Set_has(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsSet()) return TaggedValue::FromBoolean(false);
    auto* s = static_cast<JSSet*>(this_val.AsHeapObject());
    TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    return TaggedValue::FromBoolean(s->Has(v));
}

static TaggedValue Set_delete(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsSet()) return TaggedValue::FromBoolean(false);
    auto* s = static_cast<JSSet*>(this_val.AsHeapObject());
    TaggedValue v = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    return TaggedValue::FromBoolean(s->Delete(v));
}

static TaggedValue Set_clear(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsSet()) return TaggedValue::Undefined();
    auto* s = static_cast<JSSet*>(this_val.AsHeapObject());
    s->Clear();
    return TaggedValue::Undefined();
}

// Promise Intrinsics
static TaggedValue Promise_ctor(const std::vector<TaggedValue>& args) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsFunction()) {
        TaggedValue executor = args[0];
        auto* resolve_fn = AllocateRuntime<JSNativeFunction>("resolve", [p](const std::vector<TaggedValue>& r_args) {
            TaggedValue val = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
            p->Fulfill(val, CurrentMicrotaskQueue());
            return TaggedValue::Undefined();
        });
        auto* reject_fn = AllocateRuntime<JSNativeFunction>("reject", [p](const std::vector<TaggedValue>& r_args) {
            TaggedValue reason = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
            p->Reject(reason, CurrentMicrotaskQueue());
            return TaggedValue::Undefined();
        });
        CallFunction(executor, TaggedValue::Undefined(), {TaggedValue::FromHeapObject(resolve_fn), TaggedValue::FromHeapObject(reject_fn)});
    }
    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_then(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsPromise()) return TaggedValue::Undefined();
    auto* p = static_cast<JSPromise*>(this_val.AsHeapObject());
    TaggedValue on_fulfilled = args.size() > 0 ? args[0] : TaggedValue::Undefined();
    TaggedValue on_rejected = args.size() > 1 ? args[1] : TaggedValue::Undefined();

    auto* downstream = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        downstream->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    p->AddReaction(on_fulfilled, on_rejected, downstream, CurrentMicrotaskQueue());
    return TaggedValue::FromHeapObject(downstream);
}

static TaggedValue Promise_catch(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsPromise()) return TaggedValue::Undefined();
    auto* p = static_cast<JSPromise*>(this_val.AsHeapObject());
    TaggedValue on_rejected = args.size() > 0 ? args[0] : TaggedValue::Undefined();

    auto* downstream = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        downstream->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    p->AddReaction(TaggedValue::Undefined(), on_rejected, downstream, CurrentMicrotaskQueue());
    return TaggedValue::FromHeapObject(downstream);
}

static TaggedValue Promise_resolve(const std::vector<TaggedValue>& args) {
    TaggedValue val = args.empty() ? TaggedValue::Undefined() : args[0];
    if (val.IsHeapObject() && val.AsHeapObject()->IsPromise()) {
        return val;
    }
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }
    p->Fulfill(val, CurrentMicrotaskQueue());
    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_reject(const std::vector<TaggedValue>& args) {
    TaggedValue reason = args.empty() ? TaggedValue::Undefined() : args[0];
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }
    p->Reject(reason, CurrentMicrotaskQueue());
    return TaggedValue::FromHeapObject(p);
}


static TaggedValue Promise_all(const std::vector<TaggedValue>& args) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (args.empty()) {
        auto* err = AllocateRuntime<JSError>("TypeError", "Promise.all requires an iterable argument");
        p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    std::vector<TaggedValue> items;
    TaggedValue error_val = TaggedValue::Undefined();
    if (!ExtractIterableElements(args[0], items, error_val)) {
        p->Reject(error_val, CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    size_t count = items.size();
    if (count == 0) {
        auto* res_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            res_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        p->Fulfill(TaggedValue::FromHeapObject(res_arr), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    auto* results = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, count);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        results->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    auto remaining = std::make_shared<size_t>(count);
    for (size_t i = 0; i < count; ++i) {
        TaggedValue item = items[i];
        if (item.IsHeapObject() && item.AsHeapObject()->IsPromise()) {
            auto* item_p = static_cast<JSPromise*>(item.AsHeapObject());
            auto on_fulfill = AllocateRuntime<JSNativeFunction>("onFulfill", [p, results, i, remaining](const std::vector<TaggedValue>& f_args) {
                results->SetElement(i, f_args.empty() ? TaggedValue::Undefined() : f_args[0]);
                (*remaining)--;
                if (*remaining == 0) {
                    p->Fulfill(TaggedValue::FromHeapObject(results), CurrentMicrotaskQueue());
                }
                return TaggedValue::Undefined();
            });
            auto on_reject = AllocateRuntime<JSNativeFunction>("onReject", [p](const std::vector<TaggedValue>& r_args) {
                p->Reject(r_args.empty() ? TaggedValue::Undefined() : r_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            item_p->AddReaction(TaggedValue::FromHeapObject(on_fulfill), TaggedValue::FromHeapObject(on_reject), nullptr, CurrentMicrotaskQueue());
        } else {
            results->SetElement(i, item);
            (*remaining)--;
        }
    }

    if (*remaining == 0) {
        p->Fulfill(TaggedValue::FromHeapObject(results), CurrentMicrotaskQueue());
    }

    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_race(const std::vector<TaggedValue>& args) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (args.empty()) {
        auto* err = AllocateRuntime<JSError>("TypeError", "Promise.race requires an iterable argument");
        p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    std::vector<TaggedValue> items;
    TaggedValue error_val = TaggedValue::Undefined();
    if (!ExtractIterableElements(args[0], items, error_val)) {
        p->Reject(error_val, CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    size_t count = items.size();
    for (size_t i = 0; i < count; ++i) {
        TaggedValue item = items[i];
        if (item.IsHeapObject() && item.AsHeapObject()->IsPromise()) {
            auto* item_p = static_cast<JSPromise*>(item.AsHeapObject());
            auto on_fulfill = AllocateRuntime<JSNativeFunction>("onFulfill", [p](const std::vector<TaggedValue>& f_args) {
                p->Fulfill(f_args.empty() ? TaggedValue::Undefined() : f_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            auto on_reject = AllocateRuntime<JSNativeFunction>("onReject", [p](const std::vector<TaggedValue>& r_args) {
                p->Reject(r_args.empty() ? TaggedValue::Undefined() : r_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            item_p->AddReaction(TaggedValue::FromHeapObject(on_fulfill), TaggedValue::FromHeapObject(on_reject), nullptr, CurrentMicrotaskQueue());
        } else {
            p->Fulfill(item, CurrentMicrotaskQueue());
            break;
        }
    }

    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_allSettled(const std::vector<TaggedValue>& args) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (args.empty()) {
        auto* err = AllocateRuntime<JSError>("TypeError", "Promise.allSettled requires an iterable argument");
        p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    std::vector<TaggedValue> items;
    TaggedValue error_val = TaggedValue::Undefined();
    if (!ExtractIterableElements(args[0], items, error_val)) {
        p->Reject(error_val, CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    size_t count = items.size();
    if (count == 0) {
        auto* res_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            res_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        p->Fulfill(TaggedValue::FromHeapObject(res_arr), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    auto* results = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, count);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        results->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    auto remaining = std::make_shared<size_t>(count);

    for (size_t i = 0; i < count; ++i) {
        TaggedValue item = items[i];
        if (item.IsHeapObject() && item.AsHeapObject()->IsPromise()) {
            auto* item_p = static_cast<JSPromise*>(item.AsHeapObject());
            auto on_fulfill = AllocateRuntime<JSNativeFunction>("onFulfill", [p, results, i, remaining](const std::vector<TaggedValue>& f_args) {
                TaggedValue val = f_args.empty() ? TaggedValue::Undefined() : f_args[0];
                auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
                uint32_t sid = 9100;
                obj->SetNamedProperty("status", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("fulfilled")), sid);
                obj->SetNamedProperty("value", val, sid);
                results->SetElement(i, TaggedValue::FromHeapObject(obj));
                (*remaining)--;
                if (*remaining == 0) {
                    p->Fulfill(TaggedValue::FromHeapObject(results), CurrentMicrotaskQueue());
                }
                return TaggedValue::Undefined();
            });
            auto on_reject = AllocateRuntime<JSNativeFunction>("onReject", [p, results, i, remaining](const std::vector<TaggedValue>& r_args) {
                TaggedValue reason = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
                auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
                uint32_t sid = 9100;
                obj->SetNamedProperty("status", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("rejected")), sid);
                obj->SetNamedProperty("reason", reason, sid);
                results->SetElement(i, TaggedValue::FromHeapObject(obj));
                (*remaining)--;
                if (*remaining == 0) {
                    p->Fulfill(TaggedValue::FromHeapObject(results), CurrentMicrotaskQueue());
                }
                return TaggedValue::Undefined();
            });
            item_p->AddReaction(TaggedValue::FromHeapObject(on_fulfill), TaggedValue::FromHeapObject(on_reject), nullptr, CurrentMicrotaskQueue());
        } else {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            uint32_t sid = 9100;
            obj->SetNamedProperty("status", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("fulfilled")), sid);
            obj->SetNamedProperty("value", item, sid);
            results->SetElement(i, TaggedValue::FromHeapObject(obj));
            (*remaining)--;
        }
    }

    if (*remaining == 0) {
        p->Fulfill(TaggedValue::FromHeapObject(results), CurrentMicrotaskQueue());
    }

    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_any(const std::vector<TaggedValue>& args) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (args.empty()) {
        auto* err = AllocateRuntime<JSError>("TypeError", "Promise.any requires an iterable argument");
        p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    std::vector<TaggedValue> items;
    TaggedValue error_val = TaggedValue::Undefined();
    if (!ExtractIterableElements(args[0], items, error_val)) {
        p->Reject(error_val, CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    size_t count = items.size();
    if (count == 0) {
        auto* err = AllocateRuntime<JSError>("AggregateError", "All promises were rejected");
        auto* empty_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            empty_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        uint32_t sid = 9200;
        err->SetNamedProperty("errors", TaggedValue::FromHeapObject(empty_arr), sid);
        p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    auto* errors = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, count);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        errors->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    auto remaining = std::make_shared<size_t>(count);

    for (size_t i = 0; i < count; ++i) {
        TaggedValue item = items[i];
        if (item.IsHeapObject() && item.AsHeapObject()->IsPromise()) {
            auto* item_p = static_cast<JSPromise*>(item.AsHeapObject());
            auto on_fulfill = AllocateRuntime<JSNativeFunction>("onFulfill", [p](const std::vector<TaggedValue>& f_args) {
                p->Fulfill(f_args.empty() ? TaggedValue::Undefined() : f_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            auto on_reject = AllocateRuntime<JSNativeFunction>("onReject", [p, errors, i, remaining](const std::vector<TaggedValue>& r_args) {
                errors->SetElement(i, r_args.empty() ? TaggedValue::Undefined() : r_args[0]);
                (*remaining)--;
                if (*remaining == 0) {
                    auto* err = AllocateRuntime<JSError>("AggregateError", "All promises were rejected");
                    uint32_t sid = 9200;
                    err->SetNamedProperty("errors", TaggedValue::FromHeapObject(errors), sid);
                    p->Reject(TaggedValue::FromHeapObject(err), CurrentMicrotaskQueue());
                }
                return TaggedValue::Undefined();
            });
            item_p->AddReaction(TaggedValue::FromHeapObject(on_fulfill), TaggedValue::FromHeapObject(on_reject), nullptr, CurrentMicrotaskQueue());
        } else {
            p->Fulfill(item, CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(p);
        }
    }

    return TaggedValue::FromHeapObject(p);
}

static TaggedValue Promise_finally(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsPromise()) return TaggedValue::Undefined();
    auto* p = static_cast<JSPromise*>(this_val.AsHeapObject());
    TaggedValue callback = args.empty() ? TaggedValue::Undefined() : args[0];

    auto* downstream = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        downstream->SetPrototype(Realm::Current()->GetPromisePrototype());
    }

    if (!callback.IsHeapObject() || !callback.AsHeapObject()->IsFunction()) {
        p->AddReaction(callback, callback, downstream, CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(downstream);
    }

    auto on_fulfill = AllocateRuntime<JSNativeFunction>("onFinallyFulfill", [callback, downstream](const std::vector<TaggedValue>& f_args) {
        TaggedValue val = f_args.empty() ? TaggedValue::Undefined() : f_args[0];
        TaggedValue res = CallFunction(callback, TaggedValue::Undefined(), {});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            TaggedValue err = Interpreter::Current()->PendingException();
            Interpreter::Current()->ClearPendingException();
            downstream->Reject(err, CurrentMicrotaskQueue());
            return TaggedValue::Undefined();
        }
        if (res.IsHeapObject() && res.AsHeapObject()->IsPromise()) {
            auto* p_res = static_cast<JSPromise*>(res.AsHeapObject());
            auto after_p = AllocateRuntime<JSNativeFunction>("", [downstream, val](const std::vector<TaggedValue>&) {
                downstream->Fulfill(val, CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            auto after_err = AllocateRuntime<JSNativeFunction>("", [downstream](const std::vector<TaggedValue>& r_args) {
                downstream->Reject(r_args.empty() ? TaggedValue::Undefined() : r_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            p_res->AddReaction(TaggedValue::FromHeapObject(after_p), TaggedValue::FromHeapObject(after_err), nullptr, CurrentMicrotaskQueue());
        } else {
            downstream->Fulfill(val, CurrentMicrotaskQueue());
        }
        return TaggedValue::Undefined();
    });

    auto on_reject = AllocateRuntime<JSNativeFunction>("onFinallyReject", [callback, downstream](const std::vector<TaggedValue>& r_args) {
        TaggedValue reason = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
        TaggedValue res = CallFunction(callback, TaggedValue::Undefined(), {});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            TaggedValue err = Interpreter::Current()->PendingException();
            Interpreter::Current()->ClearPendingException();
            downstream->Reject(err, CurrentMicrotaskQueue());
            return TaggedValue::Undefined();
        }
        if (res.IsHeapObject() && res.AsHeapObject()->IsPromise()) {
            auto* p_res = static_cast<JSPromise*>(res.AsHeapObject());
            auto after_p = AllocateRuntime<JSNativeFunction>("", [downstream, reason](const std::vector<TaggedValue>&) {
                downstream->Reject(reason, CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            auto after_err = AllocateRuntime<JSNativeFunction>("", [downstream](const std::vector<TaggedValue>& e_args) {
                downstream->Reject(e_args.empty() ? TaggedValue::Undefined() : e_args[0], CurrentMicrotaskQueue());
                return TaggedValue::Undefined();
            });
            p_res->AddReaction(TaggedValue::FromHeapObject(after_p), TaggedValue::FromHeapObject(after_err), nullptr, CurrentMicrotaskQueue());
        } else {
            downstream->Reject(reason, CurrentMicrotaskQueue());
        }
        return TaggedValue::Undefined();
    });

    p->AddReaction(TaggedValue::FromHeapObject(on_fulfill), TaggedValue::FromHeapObject(on_reject), nullptr, CurrentMicrotaskQueue());
    return TaggedValue::FromHeapObject(downstream);
}

static TaggedValue Promise_withResolvers(const std::vector<TaggedValue>&) {
    auto* p = AllocateRuntime<JSPromise>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetPromisePrototype()) {
        p->SetPrototype(Realm::Current()->GetPromisePrototype());
    }
    auto* resolve_fn = AllocateRuntime<JSNativeFunction>("resolve", [p](const std::vector<TaggedValue>& r_args) {
        TaggedValue val = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
        p->Fulfill(val, CurrentMicrotaskQueue());
        return TaggedValue::Undefined();
    });
    auto* reject_fn = AllocateRuntime<JSNativeFunction>("reject", [p](const std::vector<TaggedValue>& r_args) {
        TaggedValue reason = r_args.empty() ? TaggedValue::Undefined() : r_args[0];
        p->Reject(reason, CurrentMicrotaskQueue());
        return TaggedValue::Undefined();
    });
    auto* res_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
        res_obj->SetPrototype(Realm::Current()->GetObjectPrototype());
    }
    uint32_t sid = 9300;
    res_obj->SetNamedProperty("promise", TaggedValue::FromHeapObject(p), sid);
    res_obj->SetNamedProperty("resolve", TaggedValue::FromHeapObject(resolve_fn), sid);
    res_obj->SetNamedProperty("reject", TaggedValue::FromHeapObject(reject_fn), sid);
    return TaggedValue::FromHeapObject(res_obj);
}

// RegExp Intrinsics
static TaggedValue RegExp_ctor(const std::vector<TaggedValue>& args) {
    std::string pattern = args.size() > 0 ? args[0].ToString() : "";
    std::string flags = args.size() > 1 ? args[1].ToString() : "";
    try {
        RegexBudget budget;
        (void)BoundedRegex::Compile(pattern, flags, budget);
    } catch (const RegexError& error) {
        SetRegexException(error);
        return TaggedValue::Undefined();
    }
    auto* re = AllocateRuntime<JSRegExp>(pattern, flags, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetRegExpPrototype()) {
        re->SetPrototype(Realm::Current()->GetRegExpPrototype());
    }
    return TaggedValue::FromHeapObject(re);
}

static TaggedValue RegExp_test(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    auto* re = static_cast<JSRegExp*>(this_val.AsHeapObject());
    std::string str = args.size() > 0 ? args[0].ToString() : "";
    return TaggedValue::FromBoolean(re->Test(str));
}

static TaggedValue RegExp_exec(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::Null();
    auto* re = static_cast<JSRegExp*>(this_val.AsHeapObject());
    std::string str = args.empty() ? "" : args[0].ToString();

    try {
        RegexBudget budget;
        BoundedRegex regex = BoundedRegex::Compile(re->Pattern(), re->Flags(), budget);
        auto match = regex.Search(str, 0, budget);
        if (!match) return TaggedValue::Null();
        auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        for (const auto& capture : match->captures) {
            if (capture.has_value()) {
                const auto range = *capture;
                arr->Push(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSString>(str.substr(range.begin, range.end - range.begin))));
            } else {
                arr->Push(TaggedValue::Undefined());
            }
        }
        uint32_t sid = 9300;
        arr->SetNamedProperty("index", TaggedValue::FromSmi(static_cast<int32_t>(match->Start())), sid);
        arr->SetNamedProperty("input", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(str)), sid);
        return TaggedValue::FromHeapObject(arr);
    } catch (const RegexError& error) {
        SetRegexException(error);
        return TaggedValue::Undefined();
    }
}

static TaggedValue RegExp_get_flags(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) {
        SetTypeException("RegExp.prototype.flags getter called on non-object");
        return TaggedValue::Undefined();
    }
    if (this_val.AsHeapObject()->IsRegExp()) {
        auto* re = static_cast<JSRegExp*>(this_val.AsHeapObject());
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(re->Flags()));
    }
    auto* obj = static_cast<JSObject*>(this_val.AsHeapObject());
    std::string result;
    for (char flag : {'d', 'g', 'i', 'm', 's', 'u', 'y'}) {
        std::string prop;
        switch (flag) {
            case 'd': prop = "hasIndices"; break;
            case 'g': prop = "global"; break;
            case 'i': prop = "ignoreCase"; break;
            case 'm': prop = "multiline"; break;
            case 's': prop = "dotAll"; break;
            case 'u': prop = "unicode"; break;
            case 'y': prop = "sticky"; break;
        }
        TaggedValue v = obj->GetNamedProperty(prop);
        if (v.ToBoolean()) result.push_back(flag);
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(result));
}

static TaggedValue RegExp_get_source(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) {
        SetTypeException("RegExp.prototype.source getter called on non-object");
        return TaggedValue::Undefined();
    }
    if (this_val.AsHeapObject()->IsRegExp()) {
        auto* re = static_cast<JSRegExp*>(this_val.AsHeapObject());
        std::string pat = re->Pattern();
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(pat.empty() ? "(?:)" : pat));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("(?:)"));
}

static TaggedValue RegExp_get_global(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('g') != std::string::npos);
}

static TaggedValue RegExp_get_ignoreCase(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('i') != std::string::npos);
}

static TaggedValue RegExp_get_multiline(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('m') != std::string::npos);
}

static TaggedValue RegExp_get_dotAll(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('s') != std::string::npos);
}

static TaggedValue RegExp_get_unicode(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('u') != std::string::npos);
}

static TaggedValue RegExp_get_sticky(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsRegExp()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(static_cast<JSRegExp*>(this_val.AsHeapObject())->Flags().find('y') != std::string::npos);
}

static TaggedValue RegExp_match(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) {
        SetTypeException("RegExp.prototype[Symbol.match] called on non-object");
        return TaggedValue::Undefined();
    }
    std::string s_str = args.empty() ? "undefined" : args[0].ToString();
    TaggedValue str_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s_str));
    auto* obj = static_cast<JSObject*>(this_val.AsHeapObject());
    TaggedValue global_val = obj->GetNamedProperty("global");
    bool global = global_val.ToBoolean();
    if (!global) {
        TaggedValue exec_fn = obj->GetNamedProperty("exec");
        if (exec_fn.IsHeapObject() && exec_fn.AsHeapObject()->IsFunction()) {
            return CallFunction(exec_fn, this_val, {str_val});
        }
        return RegExp_exec(this_val, {str_val});
    }

    uint32_t sid = 0;
    obj->SetNamedProperty("lastIndex", TaggedValue::FromSmi(0), sid);
    auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    TaggedValue exec_fn = obj->GetNamedProperty("exec");
    while (true) {
        TaggedValue result;
        if (exec_fn.IsHeapObject() && exec_fn.AsHeapObject()->IsFunction()) {
            result = CallFunction(exec_fn, this_val, {str_val});
        } else {
            result = RegExp_exec(this_val, {str_val});
        }
        if (result.IsNull()) {
            if (arr->Length() == 0) return TaggedValue::Null();
            return TaggedValue::FromHeapObject(arr);
        }
        if (!result.IsHeapObject() || !result.AsHeapObject()->IsJSObject()) {
            SetTypeException("RegExp exec returned non-object");
            return TaggedValue::Undefined();
        }
        TaggedValue match_str = static_cast<JSObject*>(result.AsHeapObject())->GetNamedProperty("0");
        std::string match = match_str.ToString();
        arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(match)));
        if (match.empty()) {
            TaggedValue last_idx = obj->GetNamedProperty("lastIndex");
            int64_t this_idx = last_idx.IsNumber() ? static_cast<int64_t>(last_idx.AsNumber()) : 0;
            obj->SetNamedProperty("lastIndex", TaggedValue::FromSmi(static_cast<int32_t>(this_idx + 1)), sid);
        }
    }
}

static TaggedValue RegExp_search(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsJSObject()) {
        SetTypeException("RegExp.prototype[Symbol.search] called on non-object");
        return TaggedValue::Undefined();
    }
    std::string s_str = args.empty() ? "undefined" : args[0].ToString();
    TaggedValue str_val = TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s_str));
    auto* obj = static_cast<JSObject*>(this_val.AsHeapObject());
    TaggedValue prev_last_index = obj->GetNamedProperty("lastIndex");
    uint32_t sid = 0;
    obj->SetNamedProperty("lastIndex", TaggedValue::FromSmi(0), sid);
    TaggedValue exec_fn = obj->GetNamedProperty("exec");
    TaggedValue result;
    if (exec_fn.IsHeapObject() && exec_fn.AsHeapObject()->IsFunction()) {
        result = CallFunction(exec_fn, this_val, {str_val});
    } else {
        result = RegExp_exec(this_val, {str_val});
    }
    obj->SetNamedProperty("lastIndex", prev_last_index, sid);
    if (result.IsNull()) return TaggedValue::FromSmi(-1);
    if (result.IsHeapObject() && result.AsHeapObject()->IsJSObject()) {
        TaggedValue idx = static_cast<JSObject*>(result.AsHeapObject())->GetNamedProperty("index");
        return idx.IsSmi() ? idx : TaggedValue::FromSmi(static_cast<int32_t>(idx.AsNumber()));
    }
    return TaggedValue::FromSmi(-1);
}

// Event Loop, Timer & Microtask Callbacks
static TaggedValue Global_setTimeout(const std::vector<TaggedValue>& args) {
    if (args.empty() || !EventLoop::Current()) return TaggedValue::FromSmi(0);
    TaggedValue callback = args[0];
    int64_t delay = 0;
    if (args.size() > 1) {
        delay = static_cast<int64_t>(args[1].AsNumber());
    }
    std::vector<TaggedValue> cb_args;
    for (size_t i = 2; i < args.size(); ++i) {
        cb_args.push_back(args[i]);
    }
    uint32_t id = EventLoop::Current()->SetTimeout(callback, delay, std::move(cb_args));
    return TaggedValue::FromSmi(static_cast<int32_t>(id));
}

static TaggedValue Global_clearTimeout(const std::vector<TaggedValue>& args) {
    if (!args.empty() && EventLoop::Current()) {
        uint32_t id = static_cast<uint32_t>(args[0].AsNumber());
        EventLoop::Current()->ClearTimeout(id);
    }
    return TaggedValue::Undefined();
}

static TaggedValue Global_setInterval(const std::vector<TaggedValue>& args) {
    if (args.empty() || !EventLoop::Current()) return TaggedValue::FromSmi(0);
    TaggedValue callback = args[0];
    int64_t interval = 1;
    if (args.size() > 1) {
        interval = static_cast<int64_t>(args[1].AsNumber());
    }
    std::vector<TaggedValue> cb_args;
    for (size_t i = 2; i < args.size(); ++i) {
        cb_args.push_back(args[i]);
    }
    uint32_t id = EventLoop::Current()->SetInterval(callback, interval, std::move(cb_args));
    return TaggedValue::FromSmi(static_cast<int32_t>(id));
}

static TaggedValue Global_clearInterval(const std::vector<TaggedValue>& args) {
    if (!args.empty() && EventLoop::Current()) {
        uint32_t id = static_cast<uint32_t>(args[0].AsNumber());
        EventLoop::Current()->ClearInterval(id);
    }
    return TaggedValue::Undefined();
}

static TaggedValue Global_setImmediate(const std::vector<TaggedValue>& args) {
    if (args.empty() || !EventLoop::Current()) return TaggedValue::FromSmi(0);
    TaggedValue callback = args[0];
    std::vector<TaggedValue> cb_args;
    for (size_t i = 1; i < args.size(); ++i) {
        cb_args.push_back(args[i]);
    }
    uint32_t id = EventLoop::Current()->SetImmediate(callback, std::move(cb_args));
    return TaggedValue::FromSmi(static_cast<int32_t>(id));
}

static TaggedValue Global_clearImmediate(const std::vector<TaggedValue>& args) {
    if (!args.empty() && EventLoop::Current()) {
        uint32_t id = static_cast<uint32_t>(args[0].AsNumber());
        EventLoop::Current()->ClearImmediate(id);
    }
    return TaggedValue::Undefined();
}

static TaggedValue Global_queueMicrotask(const std::vector<TaggedValue>& args) {
    if (!args.empty()) {
        TaggedValue cb = args[0];
        CurrentMicrotaskQueue().Enqueue([cb]() {
            CallFunction(cb, TaggedValue::Undefined(), {});
        }, {cb});
    }
    return TaggedValue::Undefined();
}

static TaggedValue Performance_now(const std::vector<TaggedValue>&) {
    if (EventLoop::Current()) {
        return TaggedValue::FromDouble(EventLoop::Current()->PerformanceNow());
    }
    return TaggedValue::FromDouble(0.0);
}

// ArrayBuffer Methods
static TaggedValue ArrayBuffer_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArrayBuffer()) return TaggedValue::Undefined();
    auto* ab = static_cast<JSArrayBuffer*>(this_val.AsHeapObject());
    int64_t begin = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : static_cast<int64_t>(ab->ByteLength());
    auto* new_ab = ab->Slice(begin, end, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        new_ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    return TaggedValue::FromHeapObject(new_ab);
}

static TaggedValue ArrayBuffer_isView(const std::vector<TaggedValue>& args) {
    if (args.empty() || !args[0].IsHeapObject()) return TaggedValue::FromBoolean(false);
    auto* ho = args[0].AsHeapObject();
    return TaggedValue::FromBoolean(ho->IsTypedArray() || ho->IsDataView());
}

// TypedArray Methods
static TaggedValue TypedArray_subarray(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) return TaggedValue::Undefined();
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t begin = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : static_cast<int64_t>(ta->Length());
    auto* sub = ta->Subarray(begin, end, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (ta->GetPrototype()) {
        sub->SetPrototype(ta->GetPrototype());
    }
    return TaggedValue::FromHeapObject(sub);
}

static TaggedValue TypedArray_set(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::Undefined();
    }
    auto* dst = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t target_offset = 0;
    if (args.size() > 1 && args[1].IsNumber()) {
        double off = args[1].AsNumber();
        if (off > 0) target_offset = static_cast<size_t>(off);
    }
    TaggedValue src_val = args[0];
    if (src_val.IsHeapObject()) {
        auto* src_ho = src_val.AsHeapObject();
        if (src_ho->IsTypedArray()) {
            auto* src_ta = static_cast<JSTypedArray*>(src_ho);
            size_t count = std::min(src_ta->Length(), dst->Length() > target_offset ? dst->Length() - target_offset : 0);
            for (size_t i = 0; i < count; ++i) {
                dst->SetElement(target_offset + i, src_ta->GetElement(i));
            }
        } else if (src_ho->IsArray()) {
            auto* src_arr = static_cast<JSArray*>(src_ho);
            size_t count = std::min(src_arr->Length(), dst->Length() > target_offset ? dst->Length() - target_offset : 0);
            for (size_t i = 0; i < count; ++i) {
                dst->SetElement(target_offset + i, src_arr->GetElement(i));
            }
        }
    }
    return TaggedValue::Undefined();
}

static TaggedValue TypedArray_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) return TaggedValue::Undefined();
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    int64_t begin = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;
    int64_t rel_begin = begin < 0 ? std::max<int64_t>(len + begin, 0) : std::min<int64_t>(begin, len);
    int64_t rel_end = end < 0 ? std::max<int64_t>(len + end, 0) : std::min<int64_t>(end, len);
    size_t new_len = static_cast<size_t>(std::max<int64_t>(rel_end - rel_begin, 0));

    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* ab = AllocateRuntime<JSArrayBuffer>(new_len * ta->BytesPerElement(), root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, new_len, root_shape);
    if (ta->GetPrototype()) {
        new_ta->SetPrototype(ta->GetPrototype());
    }
    for (size_t i = 0; i < new_len; ++i) {
        new_ta->SetElement(i, ta->GetElement(rel_begin + i));
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_fill(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) return this_val;
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue val = args[0];
    int64_t len = static_cast<int64_t>(ta->Length());
    int64_t start = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : 0;
    int64_t end = args.size() > 2 ? (args[2].IsSmi() ? args[2].AsSmi() : static_cast<int64_t>(args[2].AsNumber())) : len;
    if (start < 0) start = std::max<int64_t>(0, len + start);
    else start = std::min<int64_t>(len, start);
    if (end < 0) end = std::max<int64_t>(0, len + end);
    else end = std::min<int64_t>(len, end);
    for (int64_t i = start; i < end; ++i) {
        ta->SetElement(static_cast<size_t>(i), val);
    }
    return this_val;
}

static TaggedValue TypedArray_indexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.indexOf called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    if (len == 0 || args.empty()) return TaggedValue::FromSmi(-1);
    TaggedValue target = args[0];
    int64_t from = 0;
    if (args.size() > 1 && !args[1].IsUndefined()) {
        double d = args[1].ToDouble();
        if (!std::isnan(d)) {
            from = static_cast<int64_t>(d);
            if (from < 0) from = std::max<int64_t>(0, len + from);
        }
    }
    for (int64_t i = from; i < len; ++i) {
        TaggedValue elem = ta->GetElement(static_cast<size_t>(i));
        if (StrictEquals(elem, target)) return TaggedValue::FromDouble(static_cast<double>(i));
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue TypedArray_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.includes called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    if (len == 0 || args.empty()) return TaggedValue::FromBoolean(false);
    TaggedValue target = args[0];
    int64_t from = 0;
    if (args.size() > 1 && !args[1].IsUndefined()) {
        double d = args[1].ToDouble();
        if (!std::isnan(d)) {
            from = static_cast<int64_t>(d);
            if (from < 0) from = std::max<int64_t>(0, len + from);
        }
    }
    bool target_is_nan = target.IsNumber() && std::isnan(target.AsNumber());
    for (int64_t i = from; i < len; ++i) {
        TaggedValue elem = ta->GetElement(static_cast<size_t>(i));
        if (target_is_nan && elem.IsNumber() && std::isnan(elem.AsNumber())) return TaggedValue::FromBoolean(true);
        if (StrictEquals(elem, target)) return TaggedValue::FromBoolean(true);
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue TypedArray_reverse(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.reverse called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t n = ta->Length();
    for (size_t i = 0; i < n / 2; ++i) {
        TaggedValue tmp = ta->GetElement(i);
        ta->SetElement(i, ta->GetElement(n - 1 - i));
        ta->SetElement(n - 1 - i, tmp);
    }
    return this_val;
}

static TaggedValue TypedArray_join(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.join called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    std::string sep = (!args.empty() && !args[0].IsUndefined()) ? args[0].ToString() : ",";
    std::string res;
    for (size_t i = 0; i < ta->Length(); ++i) {
        if (i > 0) res += sep;
        res += ta->GetElement(i).ToString();
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

static TaggedValue TypedArray_forEach(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.forEach called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.forEach: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    for (size_t i = 0; i < ta->Length(); ++i) {
        CallFunction(cb, this_arg, {ta->GetElement(i), TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    }
    return TaggedValue::Undefined();
}

static TaggedValue TypedArray_map(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.map called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.map: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* ab = AllocateRuntime<JSArrayBuffer>(ta->Length() * ta->BytesPerElement(), root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, ta->Length(), root_shape);
    if (ta->GetPrototype()) {
        new_ta->SetPrototype(ta->GetPrototype());
    }
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue res = CallFunction(cb, this_arg, {ta->GetElement(i), TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        new_ta->SetElement(i, res);
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_filter(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.filter called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.filter: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    std::vector<TaggedValue> kept;
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) {
            kept.push_back(el);
        }
    }
    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    auto* ab = AllocateRuntime<JSArrayBuffer>(kept.size() * ta->BytesPerElement(), root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, kept.size(), root_shape);
    if (ta->GetPrototype()) {
        new_ta->SetPrototype(ta->GetPrototype());
    }
    for (size_t i = 0; i < kept.size(); ++i) {
        new_ta->SetElement(i, kept[i]);
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_reduce(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.reduce called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.reduce: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    TaggedValue cb = args[0];
    size_t i = 0;
    TaggedValue accum;
    if (args.size() > 1) {
        accum = args[1];
    } else {
        if (len == 0) {
            SetTypeException("Reduce of empty typed array with no initial value");
            return TaggedValue::Undefined();
        }
        accum = ta->GetElement(0);
        i = 1;
    }
    for (; i < static_cast<size_t>(len); ++i) {
        accum = CallFunction(cb, TaggedValue::Undefined(), {accum, ta->GetElement(i), TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    }
    return accum;
}

static TaggedValue TypedArray_find(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.find called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.find: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) {
            return el;
        }
    }
    return TaggedValue::Undefined();
}

static TaggedValue TypedArray_findIndex(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.findIndex called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.findIndex: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) {
            return TaggedValue::FromDouble(static_cast<double>(i));
        }
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue TypedArray_sort(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.sort called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t n = ta->Length();
    if (n <= 1) return this_val;
    std::vector<TaggedValue> elems;
    elems.reserve(n);
    for (size_t i = 0; i < n; ++i) elems.push_back(ta->GetElement(i));

    if (!args.empty() && !args[0].IsUndefined()) {
        if (!args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
            SetTypeException("The comparison function must be either a function or undefined");
            return TaggedValue::Undefined();
        }
        TaggedValue cmp_fn = args[0];
        std::stable_sort(elems.begin(), elems.end(), [cmp_fn](TaggedValue a, TaggedValue b) {
            TaggedValue r = CallFunction(cmp_fn, TaggedValue::Undefined(), {a, b});
            return r.IsNumber() && r.AsNumber() < 0;
        });
    } else {
        std::stable_sort(elems.begin(), elems.end(), [](TaggedValue a, TaggedValue b) {
            if (a.IsNumber() && b.IsNumber()) {
                double da = a.AsNumber();
                double db = b.AsNumber();
                if (std::isnan(da)) return false;
                if (std::isnan(db)) return true;
                return da < db;
            }
            return a.ToString() < b.ToString();
        });
    }
    for (size_t i = 0; i < n; ++i) ta->SetElement(i, elems[i]);
    return this_val;
}

static TaggedValue TypedArray_every(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.every called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.every: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (!res.ToBoolean()) return TaggedValue::FromBoolean(false);
    }
    return TaggedValue::FromBoolean(true);
}

static TaggedValue TypedArray_some(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.some called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.some: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) return TaggedValue::FromBoolean(true);
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue TypedArray_at(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.at called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    int64_t relative_idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].ToDouble()));
    int64_t k = relative_idx >= 0 ? relative_idx : len + relative_idx;
    if (k < 0 || k >= len) return TaggedValue::Undefined();
    return ta->GetElement(static_cast<size_t>(k));
}

static TaggedValue TypedArray_findLast(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.findLast called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.findLast: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    int64_t len = static_cast<int64_t>(ta->Length());
    for (int64_t i = len - 1; i >= 0; --i) {
        TaggedValue el = ta->GetElement(static_cast<size_t>(i));
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) return el;
    }
    return TaggedValue::Undefined();
}

static TaggedValue TypedArray_findLastIndex(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.findLastIndex called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.findLastIndex: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    int64_t len = static_cast<int64_t>(ta->Length());
    for (int64_t i = len - 1; i >= 0; --i) {
        TaggedValue el = ta->GetElement(static_cast<size_t>(i));
        TaggedValue res = CallFunction(cb, this_arg, {el, TaggedValue::FromDouble(static_cast<double>(i)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (res.ToBoolean()) return TaggedValue::FromDouble(static_cast<double>(i));
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue TypedArray_lastIndexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.lastIndexOf called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    if (len == 0 || args.empty()) return TaggedValue::FromSmi(-1);
    TaggedValue target = args[0];
    int64_t from = len - 1;
    if (args.size() > 1 && !args[1].IsUndefined()) {
        double d = args[1].ToDouble();
        if (std::isnan(d)) return TaggedValue::FromSmi(-1);
        from = static_cast<int64_t>(d);
        if (from >= 0) from = std::min(from, len - 1);
        else from = len + from;
    }
    if (from < 0) return TaggedValue::FromSmi(-1);
    for (int64_t i = from; i >= 0; --i) {
        TaggedValue el = ta->GetElement(static_cast<size_t>(i));
        if (StrictEquals(el, target)) return TaggedValue::FromDouble(static_cast<double>(i));
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue TypedArray_reduceRight(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.reduceRight called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsNativeFunction())) {
        SetTypeException("TypedArray.prototype.reduceRight: callback must be a function");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    TaggedValue cb = args[0];
    TaggedValue accum = TaggedValue::Undefined();
    int64_t k = len - 1;
    if (args.size() > 1) {
        accum = args[1];
    } else {
        if (len == 0) {
            SetTypeException("Reduce of empty typed array with no initial value");
            return TaggedValue::Undefined();
        }
        accum = ta->GetElement(static_cast<size_t>(k));
        --k;
    }
    for (; k >= 0; --k) {
        accum = CallFunction(cb, TaggedValue::Undefined(), {accum, ta->GetElement(static_cast<size_t>(k)), TaggedValue::FromDouble(static_cast<double>(k)), this_val});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    }
    return accum;
}

static TaggedValue TypedArray_copyWithin(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.copyWithin called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    int64_t target = args.empty() ? 0 : static_cast<int64_t>(args[0].ToDouble());
    int64_t start = args.size() > 1 ? static_cast<int64_t>(args[1].ToDouble()) : 0;
    int64_t end = args.size() > 2 ? static_cast<int64_t>(args[2].ToDouble()) : len;

    int64_t to = target < 0 ? std::max<int64_t>(len + target, 0) : std::min<int64_t>(target, len);
    int64_t from = start < 0 ? std::max<int64_t>(len + start, 0) : std::min<int64_t>(start, len);
    int64_t final_end = end < 0 ? std::max<int64_t>(len + end, 0) : std::min<int64_t>(end, len);

    int64_t count = std::min<int64_t>(final_end - from, len - to);
    if (count <= 0) return this_val;

    std::vector<TaggedValue> temp(count);
    for (int64_t i = 0; i < count; ++i) {
        temp[i] = ta->GetElement(static_cast<size_t>(from + i));
    }
    for (int64_t i = 0; i < count; ++i) {
        ta->SetElement(static_cast<size_t>(to + i), temp[i]);
    }
    return this_val;
}

static TaggedValue TypedArray_toReversed(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.toReversed called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t len = ta->Length();
    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    size_t elem_size = TypedArrayElementSize(ta->Kind());
    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, len, root_shape);
    if (ta->GetPrototype()) new_ta->SetPrototype(ta->GetPrototype());
    for (size_t i = 0; i < len; ++i) {
        new_ta->SetElement(i, ta->GetElement(len - 1 - i));
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_toSorted(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.toSorted called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t len = ta->Length();
    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    size_t elem_size = TypedArrayElementSize(ta->Kind());
    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, len, root_shape);
    if (ta->GetPrototype()) new_ta->SetPrototype(ta->GetPrototype());
    for (size_t i = 0; i < len; ++i) {
        new_ta->SetElement(i, ta->GetElement(i));
    }
    TypedArray_sort(TaggedValue::FromHeapObject(new_ta), args);
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_with(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.with called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(ta->Length());
    int64_t relative_idx = args.empty() ? 0 : static_cast<int64_t>(args[0].ToDouble());
    int64_t actual_idx = relative_idx >= 0 ? relative_idx : len + relative_idx;
    if (actual_idx < 0 || actual_idx >= len) {
        SetRangeException("Index out of range in TypedArray.prototype.with");
        return TaggedValue::Undefined();
    }
    TaggedValue value = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
    size_t elem_size = TypedArrayElementSize(ta->Kind());
    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
    }
    auto* new_ta = AllocateRuntime<JSTypedArray>(ta->Kind(), ab, 0, static_cast<size_t>(len), root_shape);
    if (ta->GetPrototype()) new_ta->SetPrototype(ta->GetPrototype());
    for (int64_t i = 0; i < len; ++i) {
        if (i == actual_idx) new_ta->SetElement(static_cast<size_t>(i), value);
        else new_ta->SetElement(static_cast<size_t>(i), ta->GetElement(static_cast<size_t>(i)));
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_toLocaleString(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
        SetTypeException("Method TypedArray.prototype.toLocaleString called on incompatible receiver");
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    size_t len = ta->Length();
    std::string res;
    for (size_t i = 0; i < len; ++i) {
        if (i > 0) res += ",";
        res += ta->GetElement(i).ToString();
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

// DataView Helpers & Methods
static JSDataView* ValidateDataView(TaggedValue this_val, std::string_view method_name) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView()) {
        SetTypeException(std::string("Method DataView.prototype.") + std::string(method_name) + " called on incompatible receiver");
        return nullptr;
    }
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    if (dv->Buffer() && dv->Buffer()->IsDetached()) {
        SetTypeException("Cannot perform operation on detached ArrayBuffer");
        return nullptr;
    }
    return dv;
}

static bool DataViewToIndex(TaggedValue val, size_t& out_index) {
    if (val.IsUndefined()) {
        out_index = 0;
        return true;
    }
    double d = val.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
        return false;
    }
    if (std::isnan(d)) {
        out_index = 0;
        return true;
    }
    double integer_index = std::trunc(d);
    if (integer_index < 0.0 || integer_index > 9007199254740991.0) { // 2^53 - 1
        SetRangeException("Offset is outside the bounds of the DataView");
        return false;
    }
    out_index = static_cast<size_t>(integer_index);
    return true;
}

static bool CheckDataViewBounds(JSDataView* dv, size_t offset, size_t size) {
    if (!dv || !dv->CheckBounds(offset, size)) {
        SetRangeException("Offset is outside the bounds of the DataView");
        return false;
    }
    return true;
}

static TaggedValue DataView_getInt8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getInt8");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    if (!CheckDataViewBounds(dv, offset, 1)) return TaggedValue::Undefined();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetInt8(offset)));
}

static TaggedValue DataView_setInt8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setInt8");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    if (!CheckDataViewBounds(dv, offset, 1)) return TaggedValue::Undefined();
    dv->SetInt8(offset, static_cast<int8_t>(static_cast<int32_t>(num_val)));
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getUint8");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    if (!CheckDataViewBounds(dv, offset, 1)) return TaggedValue::Undefined();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetUint8(offset)));
}

static TaggedValue DataView_setUint8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setUint8");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    if (!CheckDataViewBounds(dv, offset, 1)) return TaggedValue::Undefined();
    dv->SetUint8(offset, static_cast<uint8_t>(static_cast<uint32_t>(num_val)));
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getInt16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getInt16");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 2)) return TaggedValue::Undefined();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetInt16(offset, le)));
}

static TaggedValue DataView_setInt16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setInt16");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 2)) return TaggedValue::Undefined();
    dv->SetInt16(offset, static_cast<int16_t>(static_cast<int32_t>(num_val)), le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getUint16");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 2)) return TaggedValue::Undefined();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetUint16(offset, le)));
}

static TaggedValue DataView_setUint16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setUint16");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 2)) return TaggedValue::Undefined();
    dv->SetUint16(offset, static_cast<uint16_t>(static_cast<uint32_t>(num_val)), le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getInt32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getInt32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    return TaggedValue::FromSmi(dv->GetInt32(offset, le));
}

static TaggedValue DataView_setInt32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setInt32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    dv->SetInt32(offset, static_cast<int32_t>(num_val), le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getUint32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    uint32_t u = dv->GetUint32(offset, le);
    if (u <= 0x3FFFFFFF) return TaggedValue::FromSmi(static_cast<int32_t>(u));
    return TaggedValue::FromDouble(static_cast<double>(u));
}

static TaggedValue DataView_setUint32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setUint32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    dv->SetUint32(offset, static_cast<uint32_t>(num_val), le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getFloat32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getFloat32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    return TaggedValue::FromDouble(static_cast<double>(dv->GetFloat32(offset, le)));
}

static TaggedValue DataView_setFloat32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setFloat32");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 4)) return TaggedValue::Undefined();
    dv->SetFloat32(offset, static_cast<float>(num_val), le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getFloat64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getFloat64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    return TaggedValue::FromDouble(dv->GetFloat64(offset, le));
}

static TaggedValue DataView_setFloat64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setFloat64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    double num_val = val_arg.ToDouble();
    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    dv->SetFloat64(offset, num_val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getBigInt64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getBigInt64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    return TaggedValue::FromHeapObject(JSBigInt::Create(dv->GetBigInt64(offset, le)));
}

static TaggedValue DataView_setBigInt64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setBigInt64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    if (val_arg.IsNumber()) {
        SetTypeException("Cannot convert a Number value to a BigInt");
        return TaggedValue::Undefined();
    }
    int64_t val = JSTypedArray::TypedArrayToBigInt64(val_arg);
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    dv->SetBigInt64(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getBigUint64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "getBigUint64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    bool le = args.size() > 1 && args[1].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    return TaggedValue::FromHeapObject(JSBigInt::Create(dv->GetBigUint64(offset, le)));
}

static TaggedValue DataView_setBigUint64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    auto* dv = ValidateDataView(this_val, "setBigUint64");
    if (!dv) return TaggedValue::Undefined();
    size_t offset = 0;
    if (!DataViewToIndex(args.empty() ? TaggedValue::Undefined() : args[0], offset)) return TaggedValue::Undefined();
    TaggedValue val_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
    if (val_arg.IsNumber()) {
        SetTypeException("Cannot convert a Number value to a BigInt");
        return TaggedValue::Undefined();
    }
    uint64_t val = JSTypedArray::TypedArrayToBigUint64(val_arg);
    bool le = args.size() > 2 && args[2].ToBoolean();
    if (!CheckDataViewBounds(dv, offset, 8)) return TaggedValue::Undefined();
    dv->SetBigUint64(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue Array_values(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* obj = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsJSObject())
                ? static_cast<JSObject*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [obj, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (!obj) return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
        size_t len = 0;
        if (obj->IsArray()) {
            len = static_cast<JSArray*>(obj)->Length();
        } else {
            TaggedValue len_val = obj->GetNamedProperty("length");
            len = len_val.IsNumber() ? static_cast<size_t>(len_val.AsNumber()) : 0;
        }
        if (*idx_ptr < len) {
            size_t idx = (*idx_ptr)++;
            TaggedValue el = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(idx) : obj->GetNamedProperty(std::to_string(idx));
            return TaggedValue::FromHeapObject(CreateIterResult(el, false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (obj) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(obj));
    uint32_t sid = 5100;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Array_keys(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* obj = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsJSObject())
                ? static_cast<JSObject*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [obj, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (!obj) return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
        size_t len = 0;
        if (obj->IsArray()) {
            len = static_cast<JSArray*>(obj)->Length();
        } else {
            TaggedValue len_val = obj->GetNamedProperty("length");
            len = len_val.IsNumber() ? static_cast<size_t>(len_val.AsNumber()) : 0;
        }
        if (*idx_ptr < len) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromSmi(static_cast<int32_t>(idx)), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (obj) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(obj));
    uint32_t sid = 5101;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Array_entries(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* obj = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsJSObject())
                ? static_cast<JSObject*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [obj, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (!obj) return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
        size_t len = 0;
        if (obj->IsArray()) {
            len = static_cast<JSArray*>(obj)->Length();
        } else {
            TaggedValue len_val = obj->GetNamedProperty("length");
            len = len_val.IsNumber() ? static_cast<size_t>(len_val.AsNumber()) : 0;
        }
        if (*idx_ptr < len) {
            size_t idx = (*idx_ptr)++;
            TaggedValue el = obj->IsArray() ? static_cast<JSArray*>(obj)->GetElement(idx) : obj->GetNamedProperty(std::to_string(idx));
            auto* pair = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, 2);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                pair->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            pair->SetElement(0, TaggedValue::FromSmi(static_cast<int32_t>(idx)));
            pair->SetElement(1, el);
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromHeapObject(pair), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    uint32_t sid = 5102;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue String_iterator(TaggedValue this_val, const std::vector<TaggedValue>&) {
    std::string str = "";
    if (this_val.IsHeapObject() && this_val.AsHeapObject()->IsString()) {
        str = static_cast<JSString*>(this_val.AsHeapObject())->Value();
    }
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [str, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (*idx_ptr < str.length()) {
            size_t idx = (*idx_ptr)++;
            std::string ch(1, str[idx]);
            auto* s = AllocateRuntime<JSString>(ch);
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromHeapObject(s), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    uint32_t sid = 5103;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Map_entries(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* map = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsMap())
                ? static_cast<JSMap*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [map, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (map && *idx_ptr < map->Entries().size()) {
            size_t idx = (*idx_ptr)++;
            const auto& entry = map->Entries()[idx];
            auto* pair = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, 2);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                pair->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            pair->SetElement(0, entry.first);
            pair->SetElement(1, entry.second);
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromHeapObject(pair), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (map) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(map));
    uint32_t sid = 5104;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Map_keys(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* map = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsMap())
                ? static_cast<JSMap*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [map, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (map && *idx_ptr < map->Entries().size()) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(map->Entries()[idx].first, false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (map) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(map));
    uint32_t sid = 5105;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Map_values(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* map = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsMap())
                ? static_cast<JSMap*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [map, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (map && *idx_ptr < map->Entries().size()) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(map->Entries()[idx].second, false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (map) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(map));
    uint32_t sid = 5106;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Set_values(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* set = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSet())
                ? static_cast<JSSet*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [set, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (set && *idx_ptr < set->Values().size()) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(set->Values()[idx], false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (set) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(set));
    uint32_t sid = 5107;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue Set_entries(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* set = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSet())
                ? static_cast<JSSet*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [set, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (set && *idx_ptr < set->Values().size()) {
            size_t idx = (*idx_ptr)++;
            TaggedValue val = set->Values()[idx];
            auto* pair = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, 2);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                pair->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            pair->SetElement(0, val);
            pair->SetElement(1, val);
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromHeapObject(pair), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (set) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(set));
    uint32_t sid = 5108;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue TypedArray_values(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* ta = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsTypedArray())
               ? static_cast<JSTypedArray*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [ta, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (ta && *idx_ptr < ta->Length()) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(ta->GetElement(idx), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (ta) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(ta));
    uint32_t sid = 5109;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue TypedArray_keys(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* ta = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsTypedArray())
               ? static_cast<JSTypedArray*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [ta, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (ta && *idx_ptr < ta->Length()) {
            size_t idx = (*idx_ptr)++;
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromSmi(static_cast<int32_t>(idx)), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (ta) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(ta));
    uint32_t sid = 5110;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

static TaggedValue TypedArray_entries(TaggedValue this_val, const std::vector<TaggedValue>&) {
    auto* ta = (this_val.IsHeapObject() && this_val.AsHeapObject()->IsTypedArray())
               ? static_cast<JSTypedArray*>(this_val.AsHeapObject()) : nullptr;
    auto idx_ptr = std::make_shared<size_t>(0);
    auto* iter_obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    auto* next_fn = AllocateRuntime<JSNativeFunction>("next", [ta, idx_ptr](const std::vector<TaggedValue>&) -> TaggedValue {
        if (ta && *idx_ptr < ta->Length()) {
            size_t idx = (*idx_ptr)++;
            auto* pair = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, 2);
            if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
                pair->SetPrototype(Realm::Current()->GetArrayPrototype());
            }
            pair->SetElement(0, TaggedValue::FromSmi(static_cast<int32_t>(idx)));
            pair->SetElement(1, ta->GetElement(idx));
            return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::FromHeapObject(pair), false));
        }
        return TaggedValue::FromHeapObject(CreateIterResult(TaggedValue::Undefined(), true));
    });
    if (ta) next_fn->AddCapturedRoot(TaggedValue::FromHeapObject(ta));
    uint32_t sid = 5111;
    iter_obj->SetNamedProperty("next", TaggedValue::FromHeapObject(next_fn), sid);
    auto* self_fn = AllocateRuntime<JSNativeFunction>("[Symbol.iterator]", [iter_obj](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(iter_obj);
    });
    iter_obj->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(self_fn), sid);
    return TaggedValue::FromHeapObject(iter_obj);
}

Realm::Realm(Heap& heap) {
    heap_ = &heap;
    s_current_realm_ = this;
    root_shape_ = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());
    InitializeIntrinsics(heap);
}

Realm::~Realm() {
    if (heap_) {
        for (const auto& val : root_values_) {
            heap_->UnregisterExtraRoot(val.get());
        }
    }
    if (s_current_realm_ == this) {
        s_current_realm_ = nullptr;
    }
}

void Realm::InitializeIntrinsics(Heap& heap) {
    NoGCScope no_gc(heap);
    heap_ = &heap;
    s_current_realm_ = this;

    auto make_old_obj = [this, &heap](const char* /*tag*/ = "") -> JSObject* {
        JSObject* obj = heap.AllocateOld<JSObject>(root_shape_.get());
        auto val = std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(obj));
        heap.RegisterExtraRoot(val.get());
        root_values_.push_back(std::move(val));
        return obj;
    };

    auto make_native = [this, &heap](std::string_view name, auto cb, uint32_t length = 0) -> JSNativeFunction* {
        JSNativeFunction* fn = heap.AllocateOld<JSNativeFunction>(name, cb, nullptr, length);
        if (function_prototype_) {
            fn->SetPrototype(function_prototype_);
        }
        auto val = std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(fn));
        heap.RegisterExtraRoot(val.get());
        root_values_.push_back(std::move(val));
        return fn;
    };

    auto make_old_str = [this, &heap](std::string_view str) -> JSString* {
        JSString* s = heap.AllocateOld<JSString>(str);
        auto val = std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(s));
        heap.RegisterExtraRoot(val.get());
        root_values_.push_back(std::move(val));
        return s;
    };

    global_object_ = make_old_obj("global");
    math_object_ = make_old_obj("math");
    console_object_ = make_old_obj("console");
    json_object_ = make_old_obj("json");

    object_prototype_ = make_old_obj("object_proto");
    function_prototype_ = heap.AllocateOld<JSNativeFunction>("", [](const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::Undefined();
    });
    auto fn_proto_root = std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(function_prototype_));
    heap.RegisterExtraRoot(fn_proto_root.get());
    root_values_.push_back(std::move(fn_proto_root));
    array_prototype_ = make_old_obj();
    string_prototype_ = make_old_obj();
    map_prototype_ = make_old_obj();
    set_prototype_ = make_old_obj();
    promise_prototype_ = make_old_obj();
    regexp_prototype_ = make_old_obj();
    array_buffer_prototype_ = make_old_obj();
    typed_array_prototype_ = make_old_obj();
    data_view_prototype_ = make_old_obj();
    generator_prototype_ = make_old_obj();
    async_generator_prototype_ = make_old_obj();

    function_prototype_->SetPrototype(object_prototype_);
    array_prototype_->SetPrototype(object_prototype_);
    string_prototype_->SetPrototype(object_prototype_);
    map_prototype_->SetPrototype(object_prototype_);
    set_prototype_->SetPrototype(object_prototype_);
    promise_prototype_->SetPrototype(object_prototype_);
    regexp_prototype_->SetPrototype(object_prototype_);
    array_buffer_prototype_->SetPrototype(object_prototype_);
    typed_array_prototype_->SetPrototype(object_prototype_);
    data_view_prototype_->SetPrototype(object_prototype_);
    generator_prototype_->SetPrototype(object_prototype_);
    async_generator_prototype_->SetPrototype(object_prototype_);

    // Well-Known Symbols
    static uint32_t s_symbol_counter = 1000;
    auto make_symbol = [&heap](std::string_view desc) -> JSSymbol* {
        uint32_t id = ++s_symbol_counter;
        JSSymbol* sym = heap.AllocateOld<JSSymbol>(desc, id);
        return sym;
    };

    auto* sym_iterator = make_symbol("@@iterator");
    auto* sym_asyncIterator = make_symbol("@@asyncIterator");
    auto* sym_toStringTag = make_symbol("@@toStringTag");
    auto* sym_hasInstance = make_symbol("@@hasInstance");
    auto* sym_toPrimitive = make_symbol("@@toPrimitive");
    auto* sym_unscopables = make_symbol("@@unscopables");

    symbol_iterator_ = TaggedValue::FromHeapObject(sym_iterator);
    symbol_asyncIterator_ = TaggedValue::FromHeapObject(sym_asyncIterator);
    symbol_toStringTag_ = TaggedValue::FromHeapObject(sym_toStringTag);
    symbol_hasInstance_ = TaggedValue::FromHeapObject(sym_hasInstance);
    symbol_toPrimitive_ = TaggedValue::FromHeapObject(sym_toPrimitive);
    symbol_unscopables_ = TaggedValue::FromHeapObject(sym_unscopables);

    auto sym_iter_root = std::make_unique<TaggedValue>(symbol_iterator_);
    heap.RegisterExtraRoot(sym_iter_root.get());
    root_values_.push_back(std::move(sym_iter_root));

    auto sym_async_iter_root = std::make_unique<TaggedValue>(symbol_asyncIterator_);
    heap.RegisterExtraRoot(sym_async_iter_root.get());
    root_values_.push_back(std::move(sym_async_iter_root));

    auto sym_tag_root = std::make_unique<TaggedValue>(symbol_toStringTag_);
    heap.RegisterExtraRoot(sym_tag_root.get());
    root_values_.push_back(std::move(sym_tag_root));

    auto sym_inst_root = std::make_unique<TaggedValue>(symbol_hasInstance_);
    heap.RegisterExtraRoot(sym_inst_root.get());
    root_values_.push_back(std::move(sym_inst_root));

    auto sym_prim_root = std::make_unique<TaggedValue>(symbol_toPrimitive_);
    heap.RegisterExtraRoot(sym_prim_root.get());
    root_values_.push_back(std::move(sym_prim_root));

    auto sym_unscop_root = std::make_unique<TaggedValue>(symbol_unscopables_);
    heap.RegisterExtraRoot(sym_unscop_root.get());
    root_values_.push_back(std::move(sym_unscop_root));

    // Generator Prototype
    generator_prototype_->SetNamedProperty("next", TaggedValue::FromHeapObject(make_native("next", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            return TaggedValue::Undefined();
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        TaggedValue sent_val = args.empty() ? TaggedValue::Undefined() : args[0];
        if (Interpreter::Current()) {
            return Interpreter::Current()->ResumeGenerator(gen, sent_val);
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    generator_prototype_->SetNamedProperty("return", TaggedValue::FromHeapObject(make_native("return", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            return TaggedValue::Undefined();
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        TaggedValue ret_val = args.empty() ? TaggedValue::Undefined() : args[0];
        gen->SetState(JSGeneratorObject::State::Completed);
        return TaggedValue::FromHeapObject(CreateIterResult(ret_val, true));
    })), next_shape_id_);

    generator_prototype_->SetNamedProperty("throw", TaggedValue::FromHeapObject(make_native("throw", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            return TaggedValue::Undefined();
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        TaggedValue err_val = args.empty() ? TaggedValue::Undefined() : args[0];
        if (Interpreter::Current()) {
            return Interpreter::Current()->ResumeGenerator(gen, err_val, true);
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    auto* gen_iter_self = make_native("[Symbol.iterator]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    });
    generator_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(gen_iter_self), next_shape_id_);

    // AsyncGenerator Prototype
    auto make_promise = [this]() -> JSPromise* {
        Shape* root = GetRootShape();
        auto* p = AllocateRuntime<JSPromise>(root);
        if (GetPromisePrototype()) {
            p->SetPrototype(GetPromisePrototype());
        }
        return p;
    };

    async_generator_prototype_->SetNamedProperty("next", TaggedValue::FromHeapObject(make_native("next", [make_promise](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            auto* p = make_promise();
            p->Reject(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Method AsyncGenerator.prototype.next called on incompatible receiver")), CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(p);
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        auto* p = make_promise();
        TaggedValue sent_val = args.empty() ? TaggedValue::Undefined() : args[0];
        gen->EnqueueAsyncRequest(JSGeneratorObject::AsyncRequest::Type::Next, sent_val, p);
        if (Interpreter::Current()) {
            Interpreter::Current()->RunAsyncGeneratorStep(gen);
        }
        return TaggedValue::FromHeapObject(p);
    })), next_shape_id_);

    async_generator_prototype_->SetNamedProperty("return", TaggedValue::FromHeapObject(make_native("return", [make_promise](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            auto* p = make_promise();
            p->Reject(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Method AsyncGenerator.prototype.return called on incompatible receiver")), CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(p);
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        auto* p = make_promise();
        TaggedValue ret_val = args.empty() ? TaggedValue::Undefined() : args[0];
        gen->EnqueueAsyncRequest(JSGeneratorObject::AsyncRequest::Type::Return, ret_val, p);
        if (Interpreter::Current()) {
            Interpreter::Current()->RunAsyncGeneratorStep(gen);
        }
        return TaggedValue::FromHeapObject(p);
    })), next_shape_id_);

    async_generator_prototype_->SetNamedProperty("throw", TaggedValue::FromHeapObject(make_native("throw", [make_promise](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsGeneratorObject()) {
            auto* p = make_promise();
            p->Reject(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Method AsyncGenerator.prototype.throw called on incompatible receiver")), CurrentMicrotaskQueue());
            return TaggedValue::FromHeapObject(p);
        }
        auto* gen = static_cast<JSGeneratorObject*>(this_val.AsHeapObject());
        auto* p = make_promise();
        TaggedValue err_val = args.empty() ? TaggedValue::Undefined() : args[0];
        gen->EnqueueAsyncRequest(JSGeneratorObject::AsyncRequest::Type::Throw, err_val, p);
        if (Interpreter::Current()) {
            Interpreter::Current()->RunAsyncGeneratorStep(gen);
        }
        return TaggedValue::FromHeapObject(p);
    })), next_shape_id_);

    auto* async_gen_iter_self = make_native("[Symbol.asyncIterator]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    });
    async_generator_prototype_->SetNamedProperty("@@asyncIterator", TaggedValue::FromHeapObject(async_gen_iter_self), next_shape_id_);

    // Register Math functions & constants
    math_object_->SetNamedProperty("E", TaggedValue::FromDouble(2.71828182845904523536), next_shape_id_);
    math_object_->SetNamedProperty("LN10", TaggedValue::FromDouble(2.30258509299404568402), next_shape_id_);
    math_object_->SetNamedProperty("LN2", TaggedValue::FromDouble(0.69314718055994530942), next_shape_id_);
    math_object_->SetNamedProperty("LOG10E", TaggedValue::FromDouble(0.43429448190325182765), next_shape_id_);
    math_object_->SetNamedProperty("LOG2E", TaggedValue::FromDouble(1.44269504088896340736), next_shape_id_);
    math_object_->SetNamedProperty("PI", TaggedValue::FromDouble(3.14159265358979323846), next_shape_id_);
    math_object_->SetNamedProperty("SQRT1_2", TaggedValue::FromDouble(0.70710678118654752440), next_shape_id_);
    math_object_->SetNamedProperty("SQRT2", TaggedValue::FromDouble(1.41421356237309504880), next_shape_id_);
    for (const char* math_const : {"E", "LN10", "LN2", "LOG10E", "LOG2E", "PI", "SQRT1_2", "SQRT2"}) {
        math_object_->SetOwnPropertyAttributes(math_const, PropertyAttributes::None);
    }

    auto register_math_fn = [this, &make_native](const char* name, auto cb, uint32_t arity) {
        math_object_->SetNamedProperty(name, TaggedValue::FromHeapObject(make_native(name, cb, arity)), next_shape_id_);
        math_object_->SetOwnPropertyAttributes(name, PropertyAttributes::Writable | PropertyAttributes::Configurable);
    };
    register_math_fn("abs", &Math_abs, 1);
    register_math_fn("floor", &Math_floor, 1);
    register_math_fn("ceil", &Math_ceil, 1);
    register_math_fn("round", &Math_round, 1);
    register_math_fn("sqrt", &Math_sqrt, 1);
    register_math_fn("max", &Math_max, 2);
    register_math_fn("min", &Math_min, 2);
    register_math_fn("pow", &Math_pow, 2);
    register_math_fn("exp", &Math_exp, 1);
    register_math_fn("expm1", &Math_expm1, 1);
    register_math_fn("log", &Math_log, 1);
    register_math_fn("log10", &Math_log10, 1);
    register_math_fn("log2", &Math_log2, 1);
    register_math_fn("log1p", &Math_log1p, 1);
    register_math_fn("sin", &Math_sin, 1);
    register_math_fn("cos", &Math_cos, 1);
    register_math_fn("tan", &Math_tan, 1);
    register_math_fn("asin", &Math_asin, 1);
    register_math_fn("acos", &Math_acos, 1);
    register_math_fn("atan", &Math_atan, 1);
    register_math_fn("atan2", &Math_atan2, 2);
    register_math_fn("sinh", &Math_sinh, 1);
    register_math_fn("cosh", &Math_cosh, 1);
    register_math_fn("tanh", &Math_tanh, 1);
    register_math_fn("asinh", &Math_asinh, 1);
    register_math_fn("acosh", &Math_acosh, 1);
    register_math_fn("atanh", &Math_atanh, 1);
    register_math_fn("cbrt", &Math_cbrt, 1);
    register_math_fn("hypot", &Math_hypot, 2);
    register_math_fn("trunc", &Math_trunc, 1);
    register_math_fn("sign", &Math_sign, 1);
    register_math_fn("clz32", &Math_clz32, 1);
    register_math_fn("imul", &Math_imul, 2);
    register_math_fn("fround", &Math_fround, 1);
    register_math_fn("random", &Math_random, 0);

    math_object_->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Math")), next_shape_id_);
    math_object_->SetOwnPropertyAttributes("@@toStringTag", PropertyAttributes::Configurable);

    // Register Console functions
    console_object_->SetNamedProperty("log", TaggedValue::FromHeapObject(make_native("log", &Console_log)), next_shape_id_);

    // Array Prototype Methods
    array_prototype_->SetNamedProperty("push", TaggedValue::FromHeapObject(make_native("push", &Array_push, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("pop", TaggedValue::FromHeapObject(make_native("pop", &Array_pop, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("shift", TaggedValue::FromHeapObject(make_native("shift", &Array_shift, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("unshift", TaggedValue::FromHeapObject(make_native("unshift", &Array_unshift, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &Array_slice, 2)), next_shape_id_);
    array_prototype_->SetNamedProperty("splice", TaggedValue::FromHeapObject(make_native("splice", &Array_splice, 2)), next_shape_id_);
    array_prototype_->SetNamedProperty("sort", TaggedValue::FromHeapObject(make_native("sort", &Array_sort, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("join", TaggedValue::FromHeapObject(make_native("join", &Array_join, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("toString", TaggedValue::FromHeapObject(make_native("toString", &Array_toString, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("toLocaleString", TaggedValue::FromHeapObject(make_native("toLocaleString", &Array_toLocaleString, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("indexOf", TaggedValue::FromHeapObject(make_native("indexOf", &Array_indexOf, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("includes", TaggedValue::FromHeapObject(make_native("includes", &Array_includes, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("reverse", TaggedValue::FromHeapObject(make_native("reverse", &Array_reverse, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("concat", TaggedValue::FromHeapObject(make_native("concat", &Array_concat, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("map", TaggedValue::FromHeapObject(make_native("map", &Array_map, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("filter", TaggedValue::FromHeapObject(make_native("filter", &Array_filter, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("forEach", TaggedValue::FromHeapObject(make_native("forEach", &Array_forEach, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("reduce", TaggedValue::FromHeapObject(make_native("reduce", &Array_reduce, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("reduceRight", TaggedValue::FromHeapObject(make_native("reduceRight", &Array_reduceRight, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("some", TaggedValue::FromHeapObject(make_native("some", &Array_some, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("every", TaggedValue::FromHeapObject(make_native("every", &Array_every, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("find", TaggedValue::FromHeapObject(make_native("find", &Array_find, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("findIndex", TaggedValue::FromHeapObject(make_native("findIndex", &Array_findIndex, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("fill", TaggedValue::FromHeapObject(make_native("fill", &Array_fill, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("copyWithin", TaggedValue::FromHeapObject(make_native("copyWithin", &Array_copyWithin, 2)), next_shape_id_);
    array_prototype_->SetNamedProperty("flat", TaggedValue::FromHeapObject(make_native("flat", &Array_flat, 0)), next_shape_id_);
    array_prototype_->SetNamedProperty("flatMap", TaggedValue::FromHeapObject(make_native("flatMap", &Array_flatMap, 1)), next_shape_id_);
    array_prototype_->SetNamedProperty("at", TaggedValue::FromHeapObject(make_native("at", &Array_at, 1)), next_shape_id_);
    auto* arr_values = make_native("values", &Array_values, 0);
    auto* arr_keys = make_native("keys", &Array_keys, 0);
    auto* arr_entries = make_native("entries", &Array_entries, 0);
    array_prototype_->SetNamedProperty("values", TaggedValue::FromHeapObject(arr_values), next_shape_id_);
    array_prototype_->SetNamedProperty("keys", TaggedValue::FromHeapObject(arr_keys), next_shape_id_);
    array_prototype_->SetNamedProperty("entries", TaggedValue::FromHeapObject(arr_entries), next_shape_id_);
    array_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(arr_values), next_shape_id_);

    // Array Constructor
    auto* array_ctor = make_native("Array", [](const std::vector<TaggedValue>& a_args) -> TaggedValue {
        auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, a_args.size() == 1 && a_args[0].IsSmi() ? static_cast<size_t>(a_args[0].AsSmi()) : 0);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        if (a_args.size() > 1 || (a_args.size() == 1 && !a_args[0].IsSmi())) {
            for (const auto& a : a_args) arr->Push(a);
        }
        return TaggedValue::FromHeapObject(arr);
    }, 1);
    array_ctor->SetIsConstructor(true);
    array_ctor->SetNamedProperty("isArray", TaggedValue::FromHeapObject(make_native("isArray", &Array_isArray, 1)), next_shape_id_);
    array_ctor->SetOwnPropertyAttributes("isArray", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    array_ctor->SetNamedProperty("from", TaggedValue::FromHeapObject(make_native("from", &Array_from, 1)), next_shape_id_);
    array_ctor->SetOwnPropertyAttributes("from", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    array_ctor->SetNamedProperty("of", TaggedValue::FromHeapObject(make_native("of", &Array_of, 0)), next_shape_id_);
    array_ctor->SetOwnPropertyAttributes("of", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    array_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(array_prototype_), next_shape_id_);
    array_ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
    array_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(array_ctor), next_shape_id_);
    array_prototype_->SetNamedProperty("length", TaggedValue::FromSmi(0), next_shape_id_);
    for (const char* arr_meth : {"push", "pop", "shift", "unshift", "slice", "splice", "sort", "join", "toString", "toLocaleString", "indexOf", "includes", "reverse", "concat", "map", "filter", "forEach", "reduce", "reduceRight", "some", "every", "find", "findIndex", "fill", "copyWithin", "flat", "flatMap", "at", "values", "keys", "entries", "constructor"}) {
        array_prototype_->SetOwnPropertyAttributes(arr_meth, PropertyAttributes::Writable | PropertyAttributes::Configurable);
    }

    // String Prototype Methods
    string_prototype_->SetNamedProperty("length", TaggedValue::FromSmi(0), next_shape_id_);
    string_prototype_->SetNamedProperty("charAt", TaggedValue::FromHeapObject(make_native("charAt", &String_charAt)), next_shape_id_);
    string_prototype_->SetNamedProperty("charCodeAt", TaggedValue::FromHeapObject(make_native("charCodeAt", &String_charCodeAt)), next_shape_id_);
    string_prototype_->SetNamedProperty("concat", TaggedValue::FromHeapObject(make_native("concat", &String_concat)), next_shape_id_);
    string_prototype_->SetNamedProperty("indexOf", TaggedValue::FromHeapObject(make_native("indexOf", &String_indexOf)), next_shape_id_);
    string_prototype_->SetNamedProperty("lastIndexOf", TaggedValue::FromHeapObject(make_native("lastIndexOf", &String_lastIndexOf)), next_shape_id_);
    string_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &String_slice)), next_shape_id_);
    string_prototype_->SetNamedProperty("substring", TaggedValue::FromHeapObject(make_native("substring", &String_substring)), next_shape_id_);
    string_prototype_->SetNamedProperty("toLowerCase", TaggedValue::FromHeapObject(make_native("toLowerCase", &String_toLowerCase)), next_shape_id_);
    string_prototype_->SetNamedProperty("toUpperCase", TaggedValue::FromHeapObject(make_native("toUpperCase", &String_toUpperCase)), next_shape_id_);
    string_prototype_->SetNamedProperty("trim", TaggedValue::FromHeapObject(make_native("trim", &String_trim)), next_shape_id_);
    string_prototype_->SetNamedProperty("split", TaggedValue::FromHeapObject(make_native("split", &String_split)), next_shape_id_);
    string_prototype_->SetNamedProperty("includes", TaggedValue::FromHeapObject(make_native("includes", &String_includes)), next_shape_id_);
    string_prototype_->SetNamedProperty("replace", TaggedValue::FromHeapObject(make_native("replace", &String_replace)), next_shape_id_);
    string_prototype_->SetNamedProperty("repeat", TaggedValue::FromHeapObject(make_native("repeat", &String_repeat)), next_shape_id_);
    string_prototype_->SetNamedProperty("startsWith", TaggedValue::FromHeapObject(make_native("startsWith", &String_startsWith, 1)), next_shape_id_);
    string_prototype_->SetNamedProperty("endsWith", TaggedValue::FromHeapObject(make_native("endsWith", &String_endsWith, 1)), next_shape_id_);
    string_prototype_->SetNamedProperty("padStart", TaggedValue::FromHeapObject(make_native("padStart", &String_padStart, 1)), next_shape_id_);
    string_prototype_->SetNamedProperty("padEnd", TaggedValue::FromHeapObject(make_native("padEnd", &String_padEnd, 1)), next_shape_id_);
    string_prototype_->SetNamedProperty("codePointAt", TaggedValue::FromHeapObject(make_native("codePointAt", &String_codePointAt, 1)), next_shape_id_);
    string_prototype_->SetNamedProperty("at", TaggedValue::FromHeapObject(make_native("at", &String_at, 1)), next_shape_id_);
    auto* str_iter = make_native("values", &String_iterator);
    string_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(str_iter), next_shape_id_);

    // JSON Methods
    json_object_->SetNamedProperty("parse", TaggedValue::FromHeapObject(make_native("parse", &JSON_parse, 2)), next_shape_id_);
    json_object_->SetOwnPropertyAttributes("parse", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    json_object_->SetNamedProperty("stringify", TaggedValue::FromHeapObject(make_native("stringify", &JSON_stringify, 3)), next_shape_id_);
    json_object_->SetOwnPropertyAttributes("stringify", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    json_object_->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("JSON")), next_shape_id_);
    json_object_->SetOwnPropertyAttributes("@@toStringTag", PropertyAttributes::Configurable);

    // Map Prototype & Constructor
    map_prototype_->SetNamedProperty("set", TaggedValue::FromHeapObject(make_native("set", &Map_set)), next_shape_id_);
    map_prototype_->SetNamedProperty("get", TaggedValue::FromHeapObject(make_native("get", &Map_get)), next_shape_id_);
    map_prototype_->SetNamedProperty("has", TaggedValue::FromHeapObject(make_native("has", &Map_has)), next_shape_id_);
    map_prototype_->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_native("delete", &Map_delete)), next_shape_id_);
    map_prototype_->SetNamedProperty("clear", TaggedValue::FromHeapObject(make_native("clear", &Map_clear)), next_shape_id_);
    auto* map_entries = make_native("entries", &Map_entries);
    auto* map_keys = make_native("keys", &Map_keys);
    auto* map_values = make_native("values", &Map_values);
    map_prototype_->SetNamedProperty("entries", TaggedValue::FromHeapObject(map_entries), next_shape_id_);
    map_prototype_->SetNamedProperty("keys", TaggedValue::FromHeapObject(map_keys), next_shape_id_);
    map_prototype_->SetNamedProperty("values", TaggedValue::FromHeapObject(map_values), next_shape_id_);
    map_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(map_entries), next_shape_id_);
    auto* map_ctor = make_native("Map", &Map_ctor);
    map_ctor->SetIsConstructor(true);
    map_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(map_prototype_), next_shape_id_);
    map_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(map_ctor), next_shape_id_);

    // Set Prototype & Constructor
    set_prototype_->SetNamedProperty("add", TaggedValue::FromHeapObject(make_native("add", &Set_add)), next_shape_id_);
    set_prototype_->SetNamedProperty("has", TaggedValue::FromHeapObject(make_native("has", &Set_has)), next_shape_id_);
    set_prototype_->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_native("delete", &Set_delete)), next_shape_id_);
    set_prototype_->SetNamedProperty("clear", TaggedValue::FromHeapObject(make_native("clear", &Set_clear)), next_shape_id_);
    auto* set_values = make_native("values", &Set_values);
    auto* set_keys = make_native("keys", &Set_values);
    auto* set_entries = make_native("entries", &Set_entries);
    set_prototype_->SetNamedProperty("values", TaggedValue::FromHeapObject(set_values), next_shape_id_);
    set_prototype_->SetNamedProperty("keys", TaggedValue::FromHeapObject(set_keys), next_shape_id_);
    set_prototype_->SetNamedProperty("entries", TaggedValue::FromHeapObject(set_entries), next_shape_id_);
    set_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(set_values), next_shape_id_);
    auto* set_ctor = make_native("Set", &Set_ctor);
    set_ctor->SetIsConstructor(true);
    set_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(set_prototype_), next_shape_id_);
    set_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(set_ctor), next_shape_id_);

    // Promise Prototype & Constructor
    promise_prototype_->SetNamedProperty("then", TaggedValue::FromHeapObject(make_native("then", &Promise_then)), next_shape_id_);
    promise_prototype_->SetNamedProperty("catch", TaggedValue::FromHeapObject(make_native("catch", &Promise_catch)), next_shape_id_);
    promise_prototype_->SetNamedProperty("finally", TaggedValue::FromHeapObject(make_native("finally", &Promise_finally)), next_shape_id_);
    auto* promise_ctor = make_native("Promise", &Promise_ctor);
    promise_ctor->SetIsConstructor(true);
    promise_ctor->SetNamedProperty("resolve", TaggedValue::FromHeapObject(make_native("resolve", &Promise_resolve, 1)), next_shape_id_);
    promise_ctor->SetNamedProperty("reject", TaggedValue::FromHeapObject(make_native("reject", &Promise_reject)), next_shape_id_);
    promise_ctor->SetNamedProperty("all", TaggedValue::FromHeapObject(make_native("all", &Promise_all)), next_shape_id_);
    promise_ctor->SetNamedProperty("race", TaggedValue::FromHeapObject(make_native("race", &Promise_race)), next_shape_id_);
    promise_ctor->SetNamedProperty("allSettled", TaggedValue::FromHeapObject(make_native("allSettled", &Promise_allSettled)), next_shape_id_);
    promise_ctor->SetNamedProperty("any", TaggedValue::FromHeapObject(make_native("any", &Promise_any)), next_shape_id_);
    promise_ctor->SetNamedProperty("withResolvers", TaggedValue::FromHeapObject(make_native("withResolvers", &Promise_withResolvers)), next_shape_id_);
    promise_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(promise_prototype_), next_shape_id_);
    promise_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(promise_ctor), next_shape_id_);

    // RegExp Prototype & Constructor
    regexp_prototype_->SetNamedProperty("test", TaggedValue::FromHeapObject(make_native("test", &RegExp_test)), next_shape_id_);
    regexp_prototype_->SetNamedProperty("exec", TaggedValue::FromHeapObject(make_native("exec", &RegExp_exec)), next_shape_id_);
    auto make_re_accessor = [this, &heap](std::string_view name, auto getter_fn) {
        auto* get = heap.AllocateOld<JSNativeFunction>(std::string("get ") + std::string(name), getter_fn, nullptr, 0);
        regexp_prototype_->SetAccessor(name, TaggedValue::FromHeapObject(get), TaggedValue::Undefined(), PropertyAttributes::Configurable);
    };
    make_re_accessor("flags", &RegExp_get_flags);
    make_re_accessor("source", &RegExp_get_source);
    make_re_accessor("global", &RegExp_get_global);
    make_re_accessor("ignoreCase", &RegExp_get_ignoreCase);
    make_re_accessor("multiline", &RegExp_get_multiline);
    make_re_accessor("dotAll", &RegExp_get_dotAll);
    make_re_accessor("unicode", &RegExp_get_unicode);
    make_re_accessor("sticky", &RegExp_get_sticky);

    regexp_prototype_->SetNamedProperty("@@match", TaggedValue::FromHeapObject(make_native("[Symbol.match]", &RegExp_match, 1)), next_shape_id_);
    regexp_prototype_->SetOwnPropertyAttributes("@@match", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    regexp_prototype_->SetNamedProperty("@@search", TaggedValue::FromHeapObject(make_native("[Symbol.search]", &RegExp_search, 1)), next_shape_id_);
    regexp_prototype_->SetOwnPropertyAttributes("@@search", PropertyAttributes::Writable | PropertyAttributes::Configurable);

    auto* species_get = heap.AllocateOld<JSNativeFunction>("get [Symbol.species]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    }, nullptr, 0);

    auto* regexp_ctor = make_native("RegExp", &RegExp_ctor);
    regexp_ctor->SetIsConstructor(true);
    regexp_ctor->SetAccessor("@@species", TaggedValue::FromHeapObject(species_get), TaggedValue::Undefined(), PropertyAttributes::Configurable);
    promise_ctor->SetAccessor("@@species", TaggedValue::FromHeapObject(species_get), TaggedValue::Undefined(), PropertyAttributes::Configurable);
    array_ctor->SetAccessor("@@species", TaggedValue::FromHeapObject(species_get), TaggedValue::Undefined(), PropertyAttributes::Configurable);
    regexp_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(regexp_prototype_), next_shape_id_);
    regexp_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(regexp_ctor), next_shape_id_);

    // Attach to Global Object
    global_object_->SetNamedProperty("Math", TaggedValue::FromHeapObject(math_object_), next_shape_id_);
    global_object_->SetNamedProperty("console", TaggedValue::FromHeapObject(console_object_), next_shape_id_);
    global_object_->SetNamedProperty("print", TaggedValue::FromHeapObject(make_native("print", &Console_log)), next_shape_id_);
    global_object_->SetNamedProperty("JSON", TaggedValue::FromHeapObject(json_object_), next_shape_id_);
    global_object_->SetNamedProperty("Array", TaggedValue::FromHeapObject(array_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Map", TaggedValue::FromHeapObject(map_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Set", TaggedValue::FromHeapObject(set_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Promise", TaggedValue::FromHeapObject(promise_ctor), next_shape_id_);
    global_object_->SetNamedProperty("RegExp", TaggedValue::FromHeapObject(regexp_ctor), next_shape_id_);

    // Object Constructor & Prototype
    auto* object_ctor = make_native("Object", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsUndefined() || args[0].IsNull()) {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
                obj->SetPrototype(Realm::Current()->GetObjectPrototype());
            }
            return TaggedValue::FromHeapObject(obj);
        }
        if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            return args[0];
        }
        if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsBigInt()) {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue bigint_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("BigInt");
                if (bigint_ctor.IsHeapObject() && bigint_ctor.AsHeapObject()->IsJSObject()) {
                    TaggedValue proto = static_cast<JSObject*>(bigint_ctor.AsHeapObject())->GetNamedProperty("prototype");
                    if (proto.IsHeapObject() && proto.AsHeapObject()->IsJSObject()) {
                        obj->SetPrototype(static_cast<JSObject*>(proto.AsHeapObject()));
                    }
                }
            }
            uint32_t next_sid = 0;
            obj->SetNamedProperty("__bigint_data__", args[0], next_sid);
            return TaggedValue::FromHeapObject(obj);
        }
        if (args[0].IsSmi() || args[0].IsDouble()) {
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue num_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Number");
                if (num_ctor.IsHeapObject() && num_ctor.AsHeapObject()->IsFunction()) {
                    return ConstructObject(num_ctor, {args[0]});
                }
            }
        }
        if (args[0].IsBoolean()) {
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue bool_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Boolean");
                if (bool_ctor.IsHeapObject() && bool_ctor.AsHeapObject()->IsFunction()) {
                    return ConstructObject(bool_ctor, {args[0]});
                }
            }
        }
        if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsString()) {
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue str_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("String");
                if (str_ctor.IsHeapObject() && str_ctor.AsHeapObject()->IsFunction()) {
                    return ConstructObject(str_ctor, {args[0]});
                }
            }
        }
        if (args[0].IsSymbol()) {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue sym_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Symbol");
                if (sym_ctor.IsHeapObject() && sym_ctor.AsHeapObject()->IsJSObject()) {
                    TaggedValue proto = static_cast<JSObject*>(sym_ctor.AsHeapObject())->GetNamedProperty("prototype");
                    if (proto.IsHeapObject() && proto.AsHeapObject()->IsJSObject()) {
                        obj->SetPrototype(static_cast<JSObject*>(proto.AsHeapObject()));
                    }
                }
            }
            uint32_t next_sid = 0;
            obj->SetNamedProperty("__symbol_data__", args[0], next_sid);
            return TaggedValue::FromHeapObject(obj);
        }
        auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
            obj->SetPrototype(Realm::Current()->GetObjectPrototype());
        }
        return TaggedValue::FromHeapObject(obj);
    });
    object_ctor->SetIsConstructor(true);
    object_prototype_->SetNamedProperty("toString", TaggedValue::FromHeapObject(make_native("toString", &Object_toString, 0)), next_shape_id_);
    object_prototype_->SetNamedProperty("hasOwnProperty", TaggedValue::FromHeapObject(make_native("hasOwnProperty", &Object_hasOwnProperty, 1)), next_shape_id_);
    object_prototype_->SetNamedProperty("propertyIsEnumerable", TaggedValue::FromHeapObject(make_native("propertyIsEnumerable", &Object_propertyIsEnumerable, 1)), next_shape_id_);
    object_prototype_->SetNamedProperty("valueOf", TaggedValue::FromHeapObject(make_native("valueOf", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    }, 0)), next_shape_id_);
    auto to_object = [](TaggedValue val) -> JSObject* {
        if (!val.IsHeapObject()) {
            if (val.IsSmi() || val.IsDouble()) {
                if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                    TaggedValue num_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Number");
                    if (num_ctor.IsHeapObject() && num_ctor.AsHeapObject()->IsFunction()) {
                        TaggedValue obj = ConstructObject(num_ctor, {val});
                        if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
                    }
                }
            } else if (val.IsBoolean()) {
                if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                    TaggedValue bool_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("Boolean");
                    if (bool_ctor.IsHeapObject() && bool_ctor.AsHeapObject()->IsFunction()) {
                        TaggedValue obj = ConstructObject(bool_ctor, {val});
                        if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
                    }
                }
            }
            return nullptr;
        }
        HeapObject* ho = val.AsHeapObject();
        if (ho->IsJSObject()) return static_cast<JSObject*>(ho);
        if (ho->IsString()) {
            if (Realm::Current() && Realm::Current()->GetGlobalObject()) {
                TaggedValue str_ctor = Realm::Current()->GetGlobalObject()->GetNamedProperty("String");
                if (str_ctor.IsHeapObject() && str_ctor.AsHeapObject()->IsFunction()) {
                    TaggedValue obj = ConstructObject(str_ctor, {val});
                    if (obj.IsHeapObject() && obj.AsHeapObject()->IsJSObject()) return static_cast<JSObject*>(obj.AsHeapObject());
                }
            }
        }
        return nullptr;
    };

    object_prototype_->SetNamedProperty("isPrototypeOf", TaggedValue::FromHeapObject(make_native("isPrototypeOf", [to_object](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsJSObject()) return TaggedValue::False();
        if (this_val.IsNull() || this_val.IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Object.prototype.isPrototypeOf called on null or undefined")));
            return TaggedValue::False();
        }
        JSObject* target_obj = to_object(this_val);
        if (!target_obj) return TaggedValue::False();
        JSObject* proto = static_cast<JSObject*>(args[0].AsHeapObject())->GetPrototype();
        while (proto) {
            if (proto == target_obj) return TaggedValue::True();
            proto = proto->GetPrototype();
        }
        return TaggedValue::False();
    }, 1)), next_shape_id_);
    object_prototype_->SetNamedProperty("toLocaleString", TaggedValue::FromHeapObject(make_native("toLocaleString", [to_object](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (this_val.IsNull() || this_val.IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Object.prototype.toLocaleString called on null or undefined")));
            return TaggedValue::Undefined();
        }
        JSObject* o = to_object(this_val);
        if (!o) return TaggedValue::Undefined();
        TaggedValue to_string_fn = o->GetNamedProperty("toString");
        if (to_string_fn.IsHeapObject() && to_string_fn.AsHeapObject()->IsFunction()) {
            return CallFunction(to_string_fn, TaggedValue::FromHeapObject(o), {});
        }
        return Object_toString(TaggedValue::FromHeapObject(o), {});
    }, 0)), next_shape_id_);
    object_ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
    object_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(object_prototype_), next_shape_id_);
    object_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(object_ctor), next_shape_id_);
    for (const char* op_prop : {"toString", "toLocaleString", "valueOf", "hasOwnProperty", "isPrototypeOf", "propertyIsEnumerable", "constructor"}) {
        object_prototype_->SetOwnPropertyAttributes(op_prop, PropertyAttributes::Writable | PropertyAttributes::Configurable);
    }
    object_ctor->SetNamedProperty("create", TaggedValue::FromHeapObject(make_native("create", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || (!args[0].IsNull() && (!args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()))) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Object prototype may only be an Object or null")));
            }
            return TaggedValue::Undefined();
        }
        auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (args[0].IsNull()) {
            obj->SetPrototype(nullptr);
        } else {
            obj->SetPrototype(static_cast<JSObject*>(args[0].AsHeapObject()));
        }
        if (args.size() > 1 && !args[1].IsUndefined()) {
            return ObjectDefineProperties(TaggedValue::FromHeapObject(obj), args[1]);
        }
        return TaggedValue::FromHeapObject(obj);
    }, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("getPrototypeOf", TaggedValue::FromHeapObject(make_native("getPrototypeOf", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Null();
        }
        return GetPrototypeOf(args[0]);
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("setPrototypeOf", TaggedValue::FromHeapObject(make_native("setPrototypeOf", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        TaggedValue proto = args.size() > 1 ? args[1] : TaggedValue::Null();
        if (!proto.IsNull() && (!proto.IsHeapObject() || !proto.AsHeapObject()->IsECMAObject())) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Object prototype may only be an Object or null")));
            return TaggedValue::Undefined();
        }
        if (!SetPrototypeOf(args[0], proto)) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(
                TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot set object prototype")));
            return TaggedValue::Undefined();
        }
        return args[0];
    }, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("keys", TaggedValue::FromHeapObject(make_native("keys", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        TaggedValue all_keys = GetOwnKeys(args[0]);
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* arr = AllocateRuntime<JSArray>(root);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        if (all_keys.IsHeapObject() && all_keys.AsHeapObject()->IsArray() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            auto* k_arr = static_cast<JSArray*>(all_keys.AsHeapObject());
            auto* target = static_cast<JSObject*>(args[0].AsHeapObject());
            for (size_t i = 0; i < k_arr->Length(); ++i) {
                std::string k_str = k_arr->GetElement(i).ToString();
                if (k_str.rfind("@@", 0) == 0 || k_str.rfind("__private__", 0) == 0) continue;
                bool is_enumerable = false;
                if (target->IsProxy()) {
                    TaggedValue desc = ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(target), k_str);
                    if (desc.IsHeapObject() && desc.AsHeapObject()->IsJSObject()) {
                        TaggedValue enum_val = static_cast<JSObject*>(desc.AsHeapObject())->GetNamedProperty("enumerable");
                        is_enumerable = enum_val.IsUndefined() || enum_val.ToBoolean();
                    } else {
                        is_enumerable = true;
                    }
                } else {
                    is_enumerable = (target->GetOwnPropertyAttributes(k_str) & PropertyAttributes::Enumerable) != 0;
                }
                if (is_enumerable) {
                    arr->Push(k_arr->GetElement(i));
                }
            }
        }
        return TaggedValue::FromHeapObject(arr);
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("getOwnPropertyNames", TaggedValue::FromHeapObject(make_native("getOwnPropertyNames", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        TaggedValue all_keys = GetOwnKeys(args[0]);
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* arr = AllocateRuntime<JSArray>(root);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        if (all_keys.IsHeapObject() && all_keys.AsHeapObject()->IsArray()) {
            auto* k_arr = static_cast<JSArray*>(all_keys.AsHeapObject());
            for (size_t i = 0; i < k_arr->Length(); ++i) {
                std::string k_str = k_arr->GetElement(i).ToString();
                if (k_str.rfind("@@", 0) == 0 || k_str.rfind("__private__", 0) == 0) continue;
                arr->Push(k_arr->GetElement(i));
            }
        }
        return TaggedValue::FromHeapObject(arr);
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("getOwnPropertySymbols", TaggedValue::FromHeapObject(make_native("getOwnPropertySymbols", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        JSObject* obj = ToObject(args[0]);
        if (!obj) return TaggedValue::Undefined();
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* arr = AllocateRuntime<JSArray>(root);
        if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
            arr->SetPrototype(Realm::Current()->GetArrayPrototype());
        }
        TaggedValue all_keys = GetOwnKeys(TaggedValue::FromHeapObject(obj));
        if (all_keys.IsHeapObject() && all_keys.AsHeapObject()->IsArray()) {
            auto* k_arr = static_cast<JSArray*>(all_keys.AsHeapObject());
            for (size_t i = 0; i < k_arr->Length(); ++i) {
                TaggedValue k = k_arr->GetElement(i);
                std::string k_str = k.ToString();
                if (k.IsSymbol() || k_str.rfind("@@", 0) == 0) {
                    arr->Push(k);
                }
            }
        }
        return TaggedValue::FromHeapObject(arr);
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("defineProperty", TaggedValue::FromHeapObject(make_native("defineProperty", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Object.defineProperty called on non-object")));
            return TaggedValue::Undefined();
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            return TaggedValue::Undefined();
        }
        TaggedValue desc = args.size() > 2 ? args[2] : TaggedValue::Undefined();
        if (args[0].AsHeapObject()->IsProxy()) {
            ProxyDefineProperty(static_cast<JSProxy*>(args[0].AsHeapObject()), key, desc);
            return args[0];
        }
        if (!DefinePropertyOnObject(static_cast<JSObject*>(args[0].AsHeapObject()), key, desc)) {
            return TaggedValue::Undefined();
        }
        return args[0];
    }, 3)), next_shape_id_);
    object_ctor->SetNamedProperty("defineProperties", TaggedValue::FromHeapObject(make_native("defineProperties", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        TaggedValue target = !args.empty() ? args[0] : TaggedValue::Undefined();
        TaggedValue props = args.size() > 1 ? args[1] : TaggedValue::Undefined();
        return ObjectDefineProperties(target, props);
    }, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("getOwnPropertyDescriptor", TaggedValue::FromHeapObject(make_native("getOwnPropertyDescriptor", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        JSObject* obj = ToObject(args[0]);
        if (!obj) return TaggedValue::Undefined();
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) {
            return TaggedValue::Undefined();
        }
        if (obj->IsProxy()) {
            return ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(obj), key);
        }
        if (obj->HasOwnProperty(key)) {
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* desc = AllocateRuntime<JSObject>(root);
            uint32_t sid = 9400;
            if (const auto* accessor = obj->GetAccessor(key)) {
                desc->SetNamedProperty("get", accessor->getter, sid);
                desc->SetNamedProperty("set", accessor->setter, sid);
            } else {
                desc->SetNamedProperty("value", obj->GetNamedProperty(key), sid);
                PropertyAttributes attrs = obj->GetOwnPropertyAttributes(key);
                desc->SetNamedProperty("writable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Writable), sid);
            }
            PropertyAttributes attrs = obj->GetOwnPropertyAttributes(key);
            desc->SetNamedProperty("enumerable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Enumerable), sid);
            desc->SetNamedProperty("configurable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Configurable), sid);
            return TaggedValue::FromHeapObject(desc);
        }
        return TaggedValue::Undefined();
    }, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("getOwnPropertyDescriptors", TaggedValue::FromHeapObject(make_native("getOwnPropertyDescriptors", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        JSObject* obj = ToObject(args[0]);
        if (!obj) return TaggedValue::Undefined();
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* result = AllocateRuntime<JSObject>(root);
        if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
            result->SetPrototype(Realm::Current()->GetObjectPrototype());
        }
        TaggedValue keys_val = GetOwnKeys(TaggedValue::FromHeapObject(obj));
        if (keys_val.IsHeapObject() && keys_val.AsHeapObject()->IsArray()) {
            auto* keys_arr = static_cast<JSArray*>(keys_val.AsHeapObject());
            uint32_t sid = 9500;
            for (size_t i = 0; i < keys_arr->Length(); ++i) {
                std::string key = keys_arr->GetElement(i).ToString();
                if (key.rfind("__private__", 0) == 0) continue;
                TaggedValue desc_val = TaggedValue::Undefined();
                if (obj->IsProxy()) {
                    desc_val = ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(obj), key);
                } else if (obj->HasOwnProperty(key)) {
                    auto* desc = AllocateRuntime<JSObject>(root);
                    if (const auto* accessor = obj->GetAccessor(key)) {
                        desc->SetNamedProperty("get", accessor->getter, sid);
                        desc->SetNamedProperty("set", accessor->setter, sid);
                    } else {
                        desc->SetNamedProperty("value", obj->GetNamedProperty(key), sid);
                        PropertyAttributes attrs = obj->GetOwnPropertyAttributes(key);
                        desc->SetNamedProperty("writable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Writable), sid);
                    }
                    PropertyAttributes attrs = obj->GetOwnPropertyAttributes(key);
                    desc->SetNamedProperty("enumerable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Enumerable), sid);
                    desc->SetNamedProperty("configurable", TaggedValue::FromBoolean(attrs & PropertyAttributes::Configurable), sid);
                    desc_val = TaggedValue::FromHeapObject(desc);
                }
                if (!desc_val.IsUndefined()) {
                    result->SetNamedProperty(key, desc_val, sid);
                }
            }
        }
        return TaggedValue::FromHeapObject(result);
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("assign", TaggedValue::FromHeapObject(make_native("assign", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot convert undefined or null to object")));
            return TaggedValue::Undefined();
        }
        TaggedValue target = args[0];
        for (size_t i = 1; i < args.size(); ++i) {
            TaggedValue src = args[i];
            if (src.IsHeapObject() && src.AsHeapObject()->IsJSObject()) {
                auto* src_obj = static_cast<JSObject*>(src.AsHeapObject());
                auto keys = src_obj->GetOwnPropertyNames();
                for (const auto& k : keys) {
                    PropertyAttributes attrs = src_obj->GetOwnPropertyAttributes(k);
                    if (!(attrs & PropertyAttributes::Enumerable)) continue;
                    SetProperty(target, k, GetProperty(src, k));
                }
            }
        }
        return target;
    }, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("values", TaggedValue::FromHeapObject(make_native("values", &Object_values, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("entries", TaggedValue::FromHeapObject(make_native("entries", &Object_entries, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("fromEntries", TaggedValue::FromHeapObject(make_native("fromEntries", &Object_fromEntries, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("is", TaggedValue::FromHeapObject(make_native("is", &Object_is, 2)), next_shape_id_);
    object_ctor->SetNamedProperty("freeze", TaggedValue::FromHeapObject(make_native("freeze", &Object_freeze, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("isFrozen", TaggedValue::FromHeapObject(make_native("isFrozen", &Object_isFrozen, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("seal", TaggedValue::FromHeapObject(make_native("seal", &Object_seal, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("isSealed", TaggedValue::FromHeapObject(make_native("isSealed", &Object_isSealed, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("preventExtensions", TaggedValue::FromHeapObject(make_native("preventExtensions", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            static_cast<JSObject*>(args[0].AsHeapObject())->PreventExtensions();
        }
        return args.empty() ? TaggedValue::Undefined() : args[0];
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("isExtensible", TaggedValue::FromHeapObject(make_native("isExtensible", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsJSObject()) return TaggedValue::FromBoolean(true);
        return TaggedValue::FromBoolean(static_cast<JSObject*>(args[0].AsHeapObject())->IsExtensible());
    }, 1)), next_shape_id_);
    object_ctor->SetNamedProperty("hasOwn", TaggedValue::FromHeapObject(make_native("hasOwn", &Object_hasOwn, 2)), next_shape_id_);
    for (const char* op_prop : {
        "create", "getPrototypeOf", "setPrototypeOf", "keys", "getOwnPropertyNames",
        "getOwnPropertySymbols", "defineProperty", "defineProperties", "getOwnPropertyDescriptor",
        "getOwnPropertyDescriptors", "assign", "values", "entries", "fromEntries",
        "is", "freeze", "isFrozen", "seal", "isSealed", "preventExtensions", "isExtensible", "hasOwn"
    }) {
        object_ctor->SetOwnPropertyAttributes(op_prop, PropertyAttributes::Writable | PropertyAttributes::Configurable);
    }
    global_object_->SetNamedProperty("Object", TaggedValue::FromHeapObject(object_ctor), next_shape_id_);
    global_object_->SetNamedProperty("parseInt", TaggedValue::FromHeapObject(make_native("parseInt", &Global_parseInt)), next_shape_id_);
    global_object_->SetNamedProperty("parseFloat", TaggedValue::FromHeapObject(make_native("parseFloat", &Global_parseFloat)), next_shape_id_);
    global_object_->SetNamedProperty("eval", TaggedValue::FromHeapObject(make_native("eval", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::Undefined();
        TaggedValue arg = args[0];
        if (!arg.IsHeapObject() || !arg.AsHeapObject()->IsString()) {
            return arg;
        }
        auto* str = static_cast<JSString*>(arg.AsHeapObject());
        if (Agent::Current()) {
            TaggedValue effective_this = this_val;
            if (effective_this.IsUndefined() && Realm::Current() && Realm::Current()->GetGlobalObject()) {
                effective_this = TaggedValue::FromHeapObject(Realm::Current()->GetGlobalObject());
            }
            return Agent::Current()->EvaluateScript(str->Value(), effective_this);
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    // Function Constructor & Prototype
    function_prototype_->SetNamedProperty("length", TaggedValue::FromSmi(0), next_shape_id_);
    function_prototype_->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("")), next_shape_id_);
    function_prototype_->SetNamedProperty("toString", TaggedValue::FromHeapObject(make_native("toString", [](TaggedValue, const std::vector<TaggedValue>&) -> TaggedValue {
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("function () { [native code] }"));
    })), next_shape_id_);
    function_prototype_->SetNamedProperty("call", TaggedValue::FromHeapObject(make_native("call", &Function_call)), next_shape_id_);
    function_prototype_->SetNamedProperty("apply", TaggedValue::FromHeapObject(make_native("apply", &Function_apply)), next_shape_id_);
    function_prototype_->SetNamedProperty("bind", TaggedValue::FromHeapObject(make_native("bind", &Function_bind)), next_shape_id_);
    auto* has_instance_fn = make_native("[Symbol.hasInstance]", &Function_hasInstance);
    function_prototype_->SetNamedProperty("@@hasInstance", TaggedValue::FromHeapObject(has_instance_fn), next_shape_id_);
    function_prototype_->SetOwnPropertyAttributes("@@hasInstance", PropertyAttributes::None);
    auto* throw_type_error = make_native("%ThrowTypeError%", [](const std::vector<TaggedValue>&) -> TaggedValue {
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "'caller' and 'arguments' are restricted function properties in strict mode and for arrow functions")));
        }
        return TaggedValue::Undefined();
    });
    function_prototype_->SetAccessor("caller", TaggedValue::FromHeapObject(throw_type_error), TaggedValue::FromHeapObject(throw_type_error), PropertyAttributes::Configurable);
    function_prototype_->SetAccessor("arguments", TaggedValue::FromHeapObject(throw_type_error), TaggedValue::FromHeapObject(throw_type_error), PropertyAttributes::Configurable);
    auto* function_ctor = make_native("Function", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        std::string params;
        std::string body;
        if (!args.empty()) {
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                if (i > 0) params += ",";
                params += args[i].ToString();
            }
            body = args.back().ToString();
        }
        std::string script = "(function anonymous(" + params + "\n) {\n" + body + "\n})";
        if (Agent::Current()) {
            return Agent::Current()->EvaluateScript(script);
        }
        return TaggedValue::Undefined();
    });
    function_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(function_prototype_), next_shape_id_);
    function_ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
    function_ctor->SetIsConstructor(true);
    function_ctor->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Function")), next_shape_id_);
    function_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(function_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Function", TaggedValue::FromHeapObject(function_ctor), next_shape_id_);

    // Proxy Constructor & revocable
    auto* proxy_ctor = make_native("Proxy", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (Interpreter::Current() && Interpreter::Current()->GetNewTarget().IsUndefined()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Constructor Proxy requires 'new'")));
            return TaggedValue::Undefined();
        }
        if (args.size() < 2 || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject() ||
            !args[1].IsHeapObject() || !args[1].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot create proxy with a non-object as target or handler")));
            }
            return TaggedValue::Undefined();
        }
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* proxy = AllocateRuntime<JSProxy>(args[0], args[1], root);
        return TaggedValue::FromHeapObject(proxy);
    }, 2);
    proxy_ctor->SetIsConstructor(true);

    proxy_ctor->SetNamedProperty("revocable", TaggedValue::FromHeapObject(make_native("revocable", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.size() < 2 || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject() ||
            !args[1].IsHeapObject() || !args[1].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Cannot create proxy with a non-object as target or handler")));
            }
            return TaggedValue::Undefined();
        }
        Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* proxy = AllocateRuntime<JSProxy>(args[0], args[1], root);
        auto* revoke_fn = AllocateRuntime<JSNativeFunction>("revoke", [proxy](const std::vector<TaggedValue>&) -> TaggedValue {
            proxy->Revoke();
            return TaggedValue::Undefined();
        });
        auto* res = AllocateRuntime<JSObject>(root);
        uint32_t sid = 9300;
        res->SetNamedProperty("proxy", TaggedValue::FromHeapObject(proxy), sid);
        res->SetNamedProperty("revoke", TaggedValue::FromHeapObject(revoke_fn), sid);
        return TaggedValue::FromHeapObject(res);
    })), next_shape_id_);
    global_object_->SetNamedProperty("Proxy", TaggedValue::FromHeapObject(proxy_ctor), next_shape_id_);

    // Reflect Global Builtin Object
    auto* reflect_obj = make_old_obj();
    reflect_obj->SetNamedProperty("get", TaggedValue::FromHeapObject(make_native("get", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.get called on non-object")));
            return TaggedValue::Undefined();
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        TaggedValue receiver = args.size() > 2 ? args[2] : args[0];
        return GetProperty(args[0], key, receiver);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("set", TaggedValue::FromHeapObject(make_native("set", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.set called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        TaggedValue val = args.size() > 2 ? args[2] : TaggedValue::Undefined();
        TaggedValue receiver = args.size() > 3 ? args[3] : args[0];
        return TaggedValue::FromBoolean(SetProperty(args[0], key, val, receiver));
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("has", TaggedValue::FromHeapObject(make_native("has", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.has called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        return TaggedValue::FromBoolean(HasProperty(args[0], key));
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("deleteProperty", TaggedValue::FromHeapObject(make_native("deleteProperty", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.deleteProperty called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        return TaggedValue::FromBoolean(DeleteProperty(args[0], key));
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("apply", TaggedValue::FromHeapObject(make_native("apply", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || (!args[0].AsHeapObject()->IsFunction() && !args[0].AsHeapObject()->IsProxy())) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.apply target is not a function")));
            return TaggedValue::Undefined();
        }
        TaggedValue this_arg = args.size() > 1 ? args[1] : TaggedValue::Undefined();
        std::vector<TaggedValue> fn_args;
        if (args.size() > 2 && args[2].IsHeapObject() && args[2].AsHeapObject()->IsArray()) {
            auto* arr = static_cast<JSArray*>(args[2].AsHeapObject());
            fn_args.reserve(arr->Length());
            for (size_t i = 0; i < arr->Length(); ++i) fn_args.push_back(arr->GetElement(i));
        }
        return CallFunction(args[0], this_arg, fn_args);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("construct", TaggedValue::FromHeapObject(make_native("construct", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto is_ctor = [](TaggedValue v) -> bool {
            if (!v.IsHeapObject()) return false;
            HeapObject* ho = v.AsHeapObject();
            if (ho->IsProxy()) return true;
            if (ho->IsNativeFunction()) return static_cast<JSNativeFunction*>(ho)->IsConstructor();
            if (ho->IsUserFunction()) {
                auto* fn = static_cast<JSFunction*>(ho);
                return !fn->IsArrow() && !fn->IsAsync() && !fn->IsGenerator();
            }
            return false;
        };
        if (args.empty() || !is_ctor(args[0])) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.construct target is not a constructor")));
            return TaggedValue::Undefined();
        }
        TaggedValue new_target = args.size() > 2 ? args[2] : args[0];
        if (!is_ctor(new_target)) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.construct newTarget is not a constructor")));
            return TaggedValue::Undefined();
        }
        std::vector<TaggedValue> ctor_args;
        if (args.size() > 1 && args[1].IsHeapObject() && args[1].AsHeapObject()->IsArray()) {
            auto* arr = static_cast<JSArray*>(args[1].AsHeapObject());
            ctor_args.reserve(arr->Length());
            for (size_t i = 0; i < arr->Length(); ++i) ctor_args.push_back(arr->GetElement(i));
        }
        return ConstructObject(args[0], ctor_args, new_target);
    }, 2)), next_shape_id_);
    reflect_obj->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Reflect")), next_shape_id_);
    reflect_obj->SetOwnPropertyAttributes("@@toStringTag", PropertyAttributes::Configurable);

    reflect_obj->SetNamedProperty("getPrototypeOf", TaggedValue::FromHeapObject(make_native("getPrototypeOf", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.getPrototypeOf called on non-object")));
            return TaggedValue::Null();
        }
        return GetPrototypeOf(args[0]);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("setPrototypeOf", TaggedValue::FromHeapObject(make_native("setPrototypeOf", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.setPrototypeOf called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        TaggedValue proto = args.size() > 1 ? args[1] : TaggedValue::Null();
        return TaggedValue::FromBoolean(SetPrototypeOf(args[0], proto));
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("ownKeys", TaggedValue::FromHeapObject(make_native("ownKeys", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.ownKeys called on non-object")));
            return TaggedValue::Undefined();
        }
        return GetOwnKeys(args[0]);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("isExtensible", TaggedValue::FromHeapObject(make_native("isExtensible", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.isExtensible called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        if (args[0].AsHeapObject()->IsProxy()) return TaggedValue::FromBoolean(ProxyIsExtensible(static_cast<JSProxy*>(args[0].AsHeapObject())));
        return TaggedValue::FromBoolean(true);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("preventExtensions", TaggedValue::FromHeapObject(make_native("preventExtensions", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.preventExtensions called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        if (args[0].AsHeapObject()->IsProxy()) return TaggedValue::FromBoolean(ProxyPreventExtensions(static_cast<JSProxy*>(args[0].AsHeapObject())));
        return TaggedValue::FromBoolean(true);
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("getOwnPropertyDescriptor", TaggedValue::FromHeapObject(make_native("getOwnPropertyDescriptor", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.getOwnPropertyDescriptor called on non-object")));
            return TaggedValue::Undefined();
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        if (args[0].AsHeapObject()->IsProxy()) return ProxyGetOwnPropertyDescriptor(static_cast<JSProxy*>(args[0].AsHeapObject()), key);
        if (args[0].AsHeapObject()->IsJSObject()) {
            auto* obj = static_cast<JSObject*>(args[0].AsHeapObject());
            if (obj->HasNamedProperty(key)) {
                TaggedValue val = obj->GetNamedProperty(key);
                Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
                auto* desc = AllocateRuntime<JSObject>(root);
                uint32_t sid = 9400;
                desc->SetNamedProperty("value", val, sid);
                desc->SetNamedProperty("writable", TaggedValue::FromBoolean(true), sid);
                desc->SetNamedProperty("enumerable", TaggedValue::FromBoolean(true), sid);
                desc->SetNamedProperty("configurable", TaggedValue::FromBoolean(true), sid);
                return TaggedValue::FromHeapObject(desc);
            }
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    reflect_obj->SetNamedProperty("defineProperty", TaggedValue::FromHeapObject(make_native("defineProperty", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsECMAObject()) {
            if (Interpreter::Current()) Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Reflect.defineProperty called on non-object")));
            return TaggedValue::FromBoolean(false);
        }
        std::string key = args.size() > 1 ? ToPropertyKeyString(args[1]) : "undefined";
        TaggedValue desc = args.size() > 2 ? args[2] : TaggedValue::Undefined();
        if (args[0].AsHeapObject()->IsProxy()) {
            return TaggedValue::FromBoolean(ProxyDefineProperty(static_cast<JSProxy*>(args[0].AsHeapObject()), key, desc));
        }
        TaggedValue val = TaggedValue::Undefined();
        if (desc.IsHeapObject() && desc.AsHeapObject()->IsECMAObject()) {
            val = GetProperty(desc, "value");
        }
        return TaggedValue::FromBoolean(SetProperty(args[0], key, val));
    })), next_shape_id_);

    global_object_->SetNamedProperty("Reflect", TaggedValue::FromHeapObject(reflect_obj), next_shape_id_);

    global_object_->SetNamedProperty("globalThis", TaggedValue::FromHeapObject(global_object_), next_shape_id_);

    // Symbol Constructor & Registry
    auto* symbol_ctor = make_native("Symbol", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        std::string desc = "";
        if (!args.empty() && !args[0].IsUndefined()) {
            desc = args[0].ToString();
        }
        static uint32_t s_sym_id = 2000;
        auto* sym = AllocateRuntime<JSSymbol>(desc, ++s_sym_id);
        return TaggedValue::FromHeapObject(sym);
    });

    symbol_ctor->SetNamedProperty("iterator", symbol_iterator_, next_shape_id_);
    symbol_ctor->SetNamedProperty("asyncIterator", symbol_asyncIterator_, next_shape_id_);
    symbol_ctor->SetNamedProperty("toStringTag", symbol_toStringTag_, next_shape_id_);
    symbol_ctor->SetNamedProperty("hasInstance", symbol_hasInstance_, next_shape_id_);
    symbol_ctor->SetNamedProperty("toPrimitive", symbol_toPrimitive_, next_shape_id_);
    symbol_ctor->SetNamedProperty("unscopables", symbol_unscopables_, next_shape_id_);

    auto* symbol_proto = AllocateRuntime<JSObject>(root_shape_.get());
    symbol_proto->SetPrototype(object_prototype_);
    symbol_prototype_ = symbol_proto;
    symbol_proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(symbol_ctor), next_shape_id_);
    symbol_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(symbol_proto), next_shape_id_);

    symbol_proto->SetAccessor("description",
        TaggedValue::FromHeapObject(make_native("get description", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
            if (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSymbol()) {
                auto* sym = static_cast<JSSymbol*>(this_val.AsHeapObject());
                std::string desc = sym->Description();
                if (desc.empty()) return TaggedValue::Undefined();
                if (desc.rfind("@@", 0) == 0) desc = "Symbol." + desc.substr(2);
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(desc));
            }
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Symbol.prototype.description called on non-symbol")));
            }
            return TaggedValue::Undefined();
        })),
        TaggedValue::Undefined());

    symbol_proto->SetNamedProperty("toString", TaggedValue::FromHeapObject(make_native("toString", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSymbol()) {
            auto* sym = static_cast<JSSymbol*>(this_val.AsHeapObject());
            std::string desc = sym->Description();
            if (desc.rfind("@@", 0) == 0) desc = "Symbol." + desc.substr(2);
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Symbol(" + desc + ")"));
        }
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Symbol.prototype.toString called on non-symbol")));
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    symbol_proto->SetNamedProperty("valueOf", TaggedValue::FromHeapObject(make_native("valueOf", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSymbol()) {
            return this_val;
        }
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Symbol.prototype.valueOf called on non-symbol")));
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    symbol_proto->SetNamedProperty("@@toPrimitive", TaggedValue::FromHeapObject(make_native("@@toPrimitive", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (this_val.IsHeapObject() && this_val.AsHeapObject()->IsSymbol()) {
            return this_val;
        }
        if (Interpreter::Current()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(AllocateRuntime<JSError>("TypeError", "Symbol.prototype[@@toPrimitive] called on non-symbol")));
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    symbol_ctor->SetNamedProperty("for", TaggedValue::FromHeapObject(make_native("for", [this](const std::vector<TaggedValue>& args) -> TaggedValue {
        std::string key = args.empty() ? "undefined" : args[0].ToString();
        auto it = symbol_registry_.find(key);
        if (it != symbol_registry_.end()) {
            return TaggedValue::FromHeapObject(it->second);
        }
        static uint32_t s_registry_sym_id = 50000;
        auto* sym = AllocateRuntime<JSSymbol>(key, ++s_registry_sym_id);
        symbol_registry_[key] = sym;
        return TaggedValue::FromHeapObject(sym);
    })), next_shape_id_);

    symbol_ctor->SetNamedProperty("keyFor", TaggedValue::FromHeapObject(make_native("keyFor", [this](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsSymbol()) {
            return TaggedValue::Undefined();
        }
        auto* sym = static_cast<JSSymbol*>(args[0].AsHeapObject());
        for (const auto& [k, s] : symbol_registry_) {
            if (s == sym) {
                return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(k));
            }
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    global_object_->SetNamedProperty("Symbol", TaggedValue::FromHeapObject(symbol_ctor), next_shape_id_);

    // Event Loop, Timer & Microtask Intrinsics
    global_object_->SetNamedProperty("setTimeout", TaggedValue::FromHeapObject(make_native("setTimeout", &Global_setTimeout)), next_shape_id_);
    global_object_->SetNamedProperty("clearTimeout", TaggedValue::FromHeapObject(make_native("clearTimeout", &Global_clearTimeout)), next_shape_id_);
    global_object_->SetNamedProperty("setInterval", TaggedValue::FromHeapObject(make_native("setInterval", &Global_setInterval)), next_shape_id_);
    global_object_->SetNamedProperty("clearInterval", TaggedValue::FromHeapObject(make_native("clearInterval", &Global_clearInterval)), next_shape_id_);
    global_object_->SetNamedProperty("setImmediate", TaggedValue::FromHeapObject(make_native("setImmediate", &Global_setImmediate)), next_shape_id_);
    global_object_->SetNamedProperty("clearImmediate", TaggedValue::FromHeapObject(make_native("clearImmediate", &Global_clearImmediate)), next_shape_id_);
    global_object_->SetNamedProperty("queueMicrotask", TaggedValue::FromHeapObject(make_native("queueMicrotask", &Global_queueMicrotask)), next_shape_id_);

    // performance object
    auto* perf_obj = make_old_obj();
    perf_obj->SetNamedProperty("now", TaggedValue::FromHeapObject(make_native("now", &Performance_now)), next_shape_id_);
    global_object_->SetNamedProperty("performance", TaggedValue::FromHeapObject(perf_obj), next_shape_id_);

    // Error Constructors
    auto make_error_ctor = [this, &make_old_obj, &make_native, &make_old_str](std::string_view err_name, JSObject* parent_proto = nullptr, JSObject* parent_ctor = nullptr) -> JSNativeFunction* {
        auto* proto = make_old_obj();
        if (parent_proto) {
            proto->SetPrototype(parent_proto);
        } else if (object_prototype_) {
            proto->SetPrototype(object_prototype_);
        }
        proto->SetNamedProperty("name", TaggedValue::FromHeapObject(make_old_str(err_name)), next_shape_id_);
        proto->SetNamedProperty("message", TaggedValue::FromHeapObject(make_old_str("")), next_shape_id_);
        proto->SetNamedProperty("toString", TaggedValue::FromHeapObject(make_native("toString", [](const std::vector<TaggedValue>&) -> TaggedValue {
            return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Error"));
        })), next_shape_id_);

        std::string name_str(err_name);
        auto* ctor = make_native(err_name, [proto, name_str](const std::vector<TaggedValue>& args) -> TaggedValue {
            auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
            obj->SetPrototype(proto);
            uint32_t sid = 2000;
            obj->SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(name_str)), sid);
            std::string msg = "";
            if (!args.empty()) {
                TaggedValue a = args[0];
                if (a.IsHeapObject() && a.AsHeapObject()->IsString()) {
                    msg = static_cast<JSString*>(a.AsHeapObject())->Value();
                } else if (a.IsSmi()) {
                    msg = std::to_string(a.AsSmi());
                } else if (a.IsDouble()) {
                    msg = std::to_string(a.AsDouble());
                }
            }
            obj->SetNamedProperty("message", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(msg)), sid);
            return TaggedValue::FromHeapObject(obj);
        });
        if (parent_ctor) {
            ctor->SetPrototype(parent_ctor);
        } else if (function_prototype_) {
            ctor->SetPrototype(function_prototype_);
        }
        ctor->SetNamedProperty("name", TaggedValue::FromHeapObject(make_old_str(name_str)), next_shape_id_);
        ctor->SetIsConstructor(true);
        ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
        ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(proto), next_shape_id_);
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(ctor), next_shape_id_);
        return ctor;
    };

    auto* error_ctor = make_error_ctor("Error", object_prototype_, function_prototype_);
    JSObject* error_proto = nullptr;
    TaggedValue ep = error_ctor->GetNamedProperty("prototype");
    if (ep.IsHeapObject() && ep.AsHeapObject()->IsJSObject()) {
        error_proto = static_cast<JSObject*>(ep.AsHeapObject());
    }

    auto* type_error_ctor = make_error_ctor("TypeError", error_proto, error_ctor);
    auto* range_error_ctor = make_error_ctor("RangeError", error_proto, error_ctor);
    auto* ref_error_ctor = make_error_ctor("ReferenceError", error_proto, error_ctor);
    auto* syntax_error_ctor = make_error_ctor("SyntaxError", error_proto, error_ctor);
    auto* test262_error_ctor = make_error_ctor("Test262Error", error_proto, error_ctor);

    global_object_->SetNamedProperty("Error", TaggedValue::FromHeapObject(error_ctor), next_shape_id_);
    global_object_->SetNamedProperty("TypeError", TaggedValue::FromHeapObject(type_error_ctor), next_shape_id_);
    global_object_->SetNamedProperty("RangeError", TaggedValue::FromHeapObject(range_error_ctor), next_shape_id_);
    global_object_->SetNamedProperty("ReferenceError", TaggedValue::FromHeapObject(ref_error_ctor), next_shape_id_);
    global_object_->SetNamedProperty("SyntaxError", TaggedValue::FromHeapObject(syntax_error_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Test262Error", TaggedValue::FromHeapObject(test262_error_ctor), next_shape_id_);

    // __aether_dynamic_import intrinsic
    global_object_->SetNamedProperty("__aether_dynamic_import", TaggedValue::FromHeapObject(make_native("__aether_dynamic_import", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::Undefined();
        std::string spec = args[0].ToString();
        auto* loader = ModuleLoader::Current();
        if (!loader && Agent::Current()) {
            loader = &Agent::Current()->GetModuleLoader();
        }
        if (!loader) {
            throw std::runtime_error("ModuleLoader is not available for dynamic import");
        }
        return loader->ImportDynamic(spec);
    })), next_shape_id_);

    // isNaN and isFinite
    global_object_->SetNamedProperty("isNaN", TaggedValue::FromHeapObject(make_native("isNaN", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::FromBoolean(true);
        double d = args[0].ToDouble();
        return TaggedValue::FromBoolean(std::isnan(d));
    })), next_shape_id_);

    global_object_->SetNamedProperty("NaN", TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN()), next_shape_id_);
    global_object_->SetNamedProperty("Infinity", TaggedValue::FromDouble(std::numeric_limits<double>::infinity()), next_shape_id_);
    global_object_->SetNamedProperty("undefined", TaggedValue::Undefined(), next_shape_id_);
    global_object_->SetOwnPropertyAttributes("NaN", PropertyAttributes::None);
    global_object_->SetOwnPropertyAttributes("Infinity", PropertyAttributes::None);
    global_object_->SetOwnPropertyAttributes("undefined", PropertyAttributes::None);

    global_object_->SetNamedProperty("isFinite", TaggedValue::FromHeapObject(make_native("isFinite", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::FromBoolean(false);
        double d = args[0].ToDouble();
        return TaggedValue::FromBoolean(std::isfinite(d));
    })), next_shape_id_);

    // ArrayBuffer Prototype & Constructor
    array_buffer_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &ArrayBuffer_slice)), next_shape_id_);
    auto* ab_get_byteLength = make_native("get byteLength", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArrayBuffer()) {
            SetTypeException("Method get ArrayBuffer.prototype.byteLength called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromDouble(static_cast<double>(static_cast<JSArrayBuffer*>(this_val.AsHeapObject())->ByteLength()));
    });
    array_buffer_prototype_->SetAccessor("byteLength", TaggedValue::FromHeapObject(ab_get_byteLength), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* array_buffer_ctor = make_native("ArrayBuffer", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        size_t byte_length = 0;
        if (!args.empty() && args[0].IsNumber()) {
            double d = args[0].AsNumber();
            if (d > 0) byte_length = static_cast<size_t>(d);
        }
        auto* ab = AllocateRuntime<JSArrayBuffer>(byte_length, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
            ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
        }
        return TaggedValue::FromHeapObject(ab);
    });
    array_buffer_ctor->SetIsConstructor(true);
    array_buffer_ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
    array_buffer_ctor->SetNamedProperty("isView", TaggedValue::FromHeapObject(make_native("isView", &ArrayBuffer_isView, 1)), next_shape_id_);
    array_buffer_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(array_buffer_prototype_), next_shape_id_);
    auto* ab_species_get = heap.AllocateOld<JSNativeFunction>("get [Symbol.species]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    }, nullptr, 0);
    array_buffer_ctor->SetAccessor("@@species", TaggedValue::FromHeapObject(ab_species_get), TaggedValue::Undefined(), PropertyAttributes::Configurable);
    global_object_->SetNamedProperty("ArrayBuffer", TaggedValue::FromHeapObject(array_buffer_ctor), next_shape_id_);

    // DataView Prototype & Constructor
    data_view_prototype_->SetNamedProperty("getInt8", TaggedValue::FromHeapObject(make_native("getInt8", &DataView_getInt8)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setInt8", TaggedValue::FromHeapObject(make_native("setInt8", &DataView_setInt8)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getUint8", TaggedValue::FromHeapObject(make_native("getUint8", &DataView_getUint8)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setUint8", TaggedValue::FromHeapObject(make_native("setUint8", &DataView_setUint8)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getInt16", TaggedValue::FromHeapObject(make_native("getInt16", &DataView_getInt16)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setInt16", TaggedValue::FromHeapObject(make_native("setInt16", &DataView_setInt16)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getUint16", TaggedValue::FromHeapObject(make_native("getUint16", &DataView_getUint16)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setUint16", TaggedValue::FromHeapObject(make_native("setUint16", &DataView_setUint16)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getInt32", TaggedValue::FromHeapObject(make_native("getInt32", &DataView_getInt32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setInt32", TaggedValue::FromHeapObject(make_native("setInt32", &DataView_setInt32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getUint32", TaggedValue::FromHeapObject(make_native("getUint32", &DataView_getUint32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setUint32", TaggedValue::FromHeapObject(make_native("setUint32", &DataView_setUint32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getFloat32", TaggedValue::FromHeapObject(make_native("getFloat32", &DataView_getFloat32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setFloat32", TaggedValue::FromHeapObject(make_native("setFloat32", &DataView_setFloat32)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getFloat64", TaggedValue::FromHeapObject(make_native("getFloat64", &DataView_getFloat64)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setFloat64", TaggedValue::FromHeapObject(make_native("setFloat64", &DataView_setFloat64)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getBigInt64", TaggedValue::FromHeapObject(make_native("getBigInt64", &DataView_getBigInt64)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setBigInt64", TaggedValue::FromHeapObject(make_native("setBigInt64", &DataView_setBigInt64)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("getBigUint64", TaggedValue::FromHeapObject(make_native("getBigUint64", &DataView_getBigUint64)), next_shape_id_);
    data_view_prototype_->SetNamedProperty("setBigUint64", TaggedValue::FromHeapObject(make_native("setBigUint64", &DataView_setBigUint64)), next_shape_id_);

    auto* dv_get_buffer = make_native("get buffer", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView()) {
            SetTypeException("Method get DataView.prototype.buffer called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
        return dv->Buffer() ? TaggedValue::FromHeapObject(dv->Buffer()) : TaggedValue::Undefined();
    });
    data_view_prototype_->SetAccessor("buffer", TaggedValue::FromHeapObject(dv_get_buffer), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* dv_get_byteLength = make_native("get byteLength", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView()) {
            SetTypeException("Method get DataView.prototype.byteLength called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
        return TaggedValue::FromDouble(static_cast<double>(dv->ByteLength()));
    });
    data_view_prototype_->SetAccessor("byteLength", TaggedValue::FromHeapObject(dv_get_byteLength), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* dv_get_byteOffset = make_native("get byteOffset", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView()) {
            SetTypeException("Method get DataView.prototype.byteOffset called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
        return TaggedValue::FromDouble(static_cast<double>(dv->ByteOffset()));
    });
    data_view_prototype_->SetAccessor("byteOffset", TaggedValue::FromHeapObject(dv_get_byteOffset), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    data_view_prototype_->SetNamedProperty("@@toStringTag", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("DataView")), next_shape_id_);

    auto* data_view_ctor = make_native("DataView", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsArrayBuffer()) {
            SetTypeException("First argument to DataView constructor must be an ArrayBuffer");
            return TaggedValue::Undefined();
        }
        auto* ab = static_cast<JSArrayBuffer*>(args[0].AsHeapObject());
        size_t byte_offset = 0;
        if (args.size() > 1 && args[1].IsNumber()) {
            double bo_d = args[1].AsNumber();
            if (bo_d > 0) byte_offset = static_cast<size_t>(bo_d);
        }
        size_t byte_length = ab->ByteLength() >= byte_offset ? ab->ByteLength() - byte_offset : 0;
        if (args.size() > 2 && args[2].IsNumber()) {
            double bl_d = args[2].AsNumber();
            if (bl_d >= 0) byte_length = static_cast<size_t>(bl_d);
        }
        auto* dv = AllocateRuntime<JSDataView>(ab, byte_offset, byte_length, Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (Realm::Current() && Realm::Current()->GetDataViewPrototype()) {
            dv->SetPrototype(Realm::Current()->GetDataViewPrototype());
        }
        return TaggedValue::FromHeapObject(dv);
    });
    data_view_ctor->SetIsConstructor(true);
    data_view_ctor->SetNamedProperty("length", TaggedValue::FromSmi(1), next_shape_id_);
    data_view_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(data_view_prototype_), next_shape_id_);
    global_object_->SetNamedProperty("DataView", TaggedValue::FromHeapObject(data_view_ctor), next_shape_id_);

    // TypedArray Shared Prototype
    typed_array_prototype_->SetNamedProperty("set", TaggedValue::FromHeapObject(make_native("set", &TypedArray_set)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("subarray", TaggedValue::FromHeapObject(make_native("subarray", &TypedArray_subarray)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &TypedArray_slice)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("fill", TaggedValue::FromHeapObject(make_native("fill", &TypedArray_fill)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("indexOf", TaggedValue::FromHeapObject(make_native("indexOf", &TypedArray_indexOf)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("includes", TaggedValue::FromHeapObject(make_native("includes", &TypedArray_includes)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("join", TaggedValue::FromHeapObject(make_native("join", &TypedArray_join)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("reverse", TaggedValue::FromHeapObject(make_native("reverse", &TypedArray_reverse)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("forEach", TaggedValue::FromHeapObject(make_native("forEach", &TypedArray_forEach)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("map", TaggedValue::FromHeapObject(make_native("map", &TypedArray_map)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("filter", TaggedValue::FromHeapObject(make_native("filter", &TypedArray_filter)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("reduce", TaggedValue::FromHeapObject(make_native("reduce", &TypedArray_reduce)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("reduceRight", TaggedValue::FromHeapObject(make_native("reduceRight", &TypedArray_reduceRight)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("find", TaggedValue::FromHeapObject(make_native("find", &TypedArray_find)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("findIndex", TaggedValue::FromHeapObject(make_native("findIndex", &TypedArray_findIndex)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("findLast", TaggedValue::FromHeapObject(make_native("findLast", &TypedArray_findLast)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("findLastIndex", TaggedValue::FromHeapObject(make_native("findLastIndex", &TypedArray_findLastIndex)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("lastIndexOf", TaggedValue::FromHeapObject(make_native("lastIndexOf", &TypedArray_lastIndexOf)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("sort", TaggedValue::FromHeapObject(make_native("sort", &TypedArray_sort)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("every", TaggedValue::FromHeapObject(make_native("every", &TypedArray_every)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("some", TaggedValue::FromHeapObject(make_native("some", &TypedArray_some)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("at", TaggedValue::FromHeapObject(make_native("at", &TypedArray_at)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("copyWithin", TaggedValue::FromHeapObject(make_native("copyWithin", &TypedArray_copyWithin)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("toReversed", TaggedValue::FromHeapObject(make_native("toReversed", &TypedArray_toReversed)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("toSorted", TaggedValue::FromHeapObject(make_native("toSorted", &TypedArray_toSorted)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("with", TaggedValue::FromHeapObject(make_native("with", &TypedArray_with)), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("toLocaleString", TaggedValue::FromHeapObject(make_native("toLocaleString", &TypedArray_toLocaleString)), next_shape_id_);
    if (array_prototype_) {
        typed_array_prototype_->SetNamedProperty("toString", array_prototype_->GetNamedProperty("toString"), next_shape_id_);
        typed_array_prototype_->SetOwnPropertyAttributes("toString", PropertyAttributes::Writable | PropertyAttributes::Configurable);
    }
    auto* ta_values = make_native("values", &TypedArray_values);
    auto* ta_keys = make_native("keys", &TypedArray_keys);
    auto* ta_entries = make_native("entries", &TypedArray_entries);
    typed_array_prototype_->SetNamedProperty("values", TaggedValue::FromHeapObject(ta_values), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("keys", TaggedValue::FromHeapObject(ta_keys), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("entries", TaggedValue::FromHeapObject(ta_entries), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(ta_values), next_shape_id_);

    // TypedArray Prototype Accessors
    auto* ta_get_length = make_native("get length", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
            SetTypeException("Method get TypedArray.prototype.length called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<JSTypedArray*>(this_val.AsHeapObject())->Length()));
    });
    typed_array_prototype_->SetAccessor("length", TaggedValue::FromHeapObject(ta_get_length), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* ta_get_byteLength = make_native("get byteLength", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
            SetTypeException("Method get TypedArray.prototype.byteLength called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromDouble(static_cast<double>(static_cast<JSTypedArray*>(this_val.AsHeapObject())->ByteLength()));
    });
    typed_array_prototype_->SetAccessor("byteLength", TaggedValue::FromHeapObject(ta_get_byteLength), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* ta_get_byteOffset = make_native("get byteOffset", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
            SetTypeException("Method get TypedArray.prototype.byteOffset called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        return TaggedValue::FromDouble(static_cast<double>(static_cast<JSTypedArray*>(this_val.AsHeapObject())->ByteOffset()));
    });
    typed_array_prototype_->SetAccessor("byteOffset", TaggedValue::FromHeapObject(ta_get_byteOffset), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* ta_get_buffer = make_native("get buffer", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
            SetTypeException("Method get TypedArray.prototype.buffer called on incompatible receiver");
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
        return ta->Buffer() ? TaggedValue::FromHeapObject(ta->Buffer()) : TaggedValue::Undefined();
    });
    typed_array_prototype_->SetAccessor("buffer", TaggedValue::FromHeapObject(ta_get_buffer), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* ta_get_toStringTag = make_native("get [Symbol.toStringTag]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) {
            return TaggedValue::Undefined();
        }
        auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
        switch (ta->Kind()) {
            case TypedArrayKind::Int8: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Int8Array"));
            case TypedArrayKind::Uint8: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Uint8Array"));
            case TypedArrayKind::Uint8Clamped: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Uint8ClampedArray"));
            case TypedArrayKind::Int16: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Int16Array"));
            case TypedArrayKind::Uint16: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Uint16Array"));
            case TypedArrayKind::Int32: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Int32Array"));
            case TypedArrayKind::Uint32: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Uint32Array"));
            case TypedArrayKind::Float32: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Float32Array"));
            case TypedArrayKind::Float64: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("Float64Array"));
            case TypedArrayKind::BigInt64: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("BigInt64Array"));
            case TypedArrayKind::BigUint64: return TaggedValue::FromHeapObject(AllocateRuntime<JSString>("BigUint64Array"));
            default: return TaggedValue::Undefined();
        }
    });
    typed_array_prototype_->SetAccessor("@@toStringTag", TaggedValue::FromHeapObject(ta_get_toStringTag), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    // %TypedArray% Intrinsics Constructor
    auto* typed_array_ctor = make_native("TypedArray", [](const std::vector<TaggedValue>&) -> TaggedValue {
        SetTypeException("TypedArray constructor cannot be called directly");
        return TaggedValue::Undefined();
    });
    typed_array_ctor->SetIsConstructor(true);
    typed_array_ctor->SetNamedProperty("length", TaggedValue::FromSmi(0), next_shape_id_);
    typed_array_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(typed_array_prototype_), next_shape_id_);
    typed_array_ctor->SetOwnPropertyAttributes("prototype", PropertyAttributes::None);
    typed_array_prototype_->SetNamedProperty("constructor", TaggedValue::FromHeapObject(typed_array_ctor), next_shape_id_);
    typed_array_prototype_->SetOwnPropertyAttributes("constructor", PropertyAttributes::Writable | PropertyAttributes::Configurable);

    auto* ta_species_get = make_native("get [Symbol.species]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    });
    typed_array_ctor->SetAccessor("@@species", TaggedValue::FromHeapObject(ta_species_get), TaggedValue::Undefined(), PropertyAttributes::Configurable);

    auto* ta_of = make_native("of", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || (!this_val.AsHeapObject()->IsFunction() && !this_val.AsHeapObject()->IsNativeFunction())) {
            SetTypeException("TypedArray.of: this is not a constructor");
            return TaggedValue::Undefined();
        }
        size_t len = args.size();
        TaggedValue new_ta_val = CallFunction(this_val, TaggedValue::Undefined(), {TaggedValue::FromDouble(static_cast<double>(len))});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (new_ta_val.IsHeapObject() && new_ta_val.AsHeapObject()->IsTypedArray()) {
            auto* ta = static_cast<JSTypedArray*>(new_ta_val.AsHeapObject());
            for (size_t i = 0; i < len; ++i) {
                ta->SetElement(i, args[i]);
            }
        }
        return new_ta_val;
    });
    typed_array_ctor->SetNamedProperty("of", TaggedValue::FromHeapObject(ta_of), next_shape_id_);
    typed_array_ctor->SetOwnPropertyAttributes("of", PropertyAttributes::Writable | PropertyAttributes::Configurable);

    auto* ta_from = make_native("from", [](TaggedValue this_val, const std::vector<TaggedValue>& args) -> TaggedValue {
        if (!this_val.IsHeapObject() || (!this_val.AsHeapObject()->IsFunction() && !this_val.AsHeapObject()->IsNativeFunction())) {
            SetTypeException("TypedArray.from: this is not a constructor");
            return TaggedValue::Undefined();
        }
        if (args.empty() || args[0].IsNull() || args[0].IsUndefined()) {
            SetTypeException("TypedArray.from requires an array-like or iterable object");
            return TaggedValue::Undefined();
        }
        TaggedValue source = args[0];
        TaggedValue map_fn = args.size() > 1 ? args[1] : TaggedValue::Undefined();
        TaggedValue this_arg = args.size() > 2 ? args[2] : TaggedValue::Undefined();
        bool mapping = !map_fn.IsUndefined();
        if (mapping && (!map_fn.IsHeapObject() || (!map_fn.AsHeapObject()->IsFunction() && !map_fn.AsHeapObject()->IsNativeFunction()))) {
            SetTypeException("TypedArray.from: mapfn must be a function");
            return TaggedValue::Undefined();
        }

        std::vector<TaggedValue> items;
        TaggedValue iter_fn = GetProperty(source, "@@iterator");
        if (iter_fn.IsHeapObject() && (iter_fn.AsHeapObject()->IsFunction() || iter_fn.AsHeapObject()->IsNativeFunction())) {
            TaggedValue iter = CallFunction(iter_fn, source, {});
            if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
            while (true) {
                TaggedValue next_res = CallFunction(GetProperty(iter, "next"), iter, {});
                if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
                if (GetProperty(next_res, "done").ToBoolean()) break;
                items.push_back(GetProperty(next_res, "value"));
            }
        } else {
            uint64_t len = ToLength(GetProperty(source, "length"));
            items.reserve(static_cast<size_t>(len));
            for (uint64_t i = 0; i < len; ++i) {
                items.push_back(GetProperty(source, std::to_string(i)));
            }
        }

        TaggedValue new_ta_val = CallFunction(this_val, TaggedValue::Undefined(), {TaggedValue::FromDouble(static_cast<double>(items.size()))});
        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
        if (new_ta_val.IsHeapObject() && new_ta_val.AsHeapObject()->IsTypedArray()) {
            auto* ta = static_cast<JSTypedArray*>(new_ta_val.AsHeapObject());
            for (size_t i = 0; i < items.size(); ++i) {
                TaggedValue val = items[i];
                if (mapping) {
                    val = CallFunction(map_fn, this_arg, {val, TaggedValue::FromDouble(static_cast<double>(i))});
                    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
                }
                ta->SetElement(i, val);
            }
        }
        return new_ta_val;
    });
    typed_array_ctor->SetNamedProperty("from", TaggedValue::FromHeapObject(ta_from), next_shape_id_);
    typed_array_ctor->SetOwnPropertyAttributes("from", PropertyAttributes::Writable | PropertyAttributes::Configurable);

    auto make_typed_array_ctor = [&](TypedArrayKind kind, std::string_view name) -> JSNativeFunction* {
        auto* proto = make_old_obj();
        proto->SetPrototype(typed_array_prototype_);
        size_t elem_size = TypedArrayElementSize(kind);
        proto->SetNamedProperty("BYTES_PER_ELEMENT", TaggedValue::FromSmi(static_cast<int32_t>(elem_size)), next_shape_id_);
        proto->SetOwnPropertyAttributes("BYTES_PER_ELEMENT", PropertyAttributes::None);

        auto* ctor = make_native(name, [this, kind, elem_size, proto](const std::vector<TaggedValue>& args) -> TaggedValue {
            Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            if (args.empty()) {
                auto* ab = AllocateRuntime<JSArrayBuffer>(0, root_shape);
                if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                    ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                }
                auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, 0, root_shape);
                ta->SetPrototype(proto);
                return TaggedValue::FromHeapObject(ta);
            }

            TaggedValue first = args[0];
            if (first.IsNumber()) {
                double len_d = first.AsNumber();
                size_t len = len_d > 0.0 ? static_cast<size_t>(len_d) : 0;
                auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
                if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                    ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                }
                auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, len, root_shape);
                ta->SetPrototype(proto);
                return TaggedValue::FromHeapObject(ta);
            }

            if (first.IsHeapObject()) {
                HeapObject* ho = first.AsHeapObject();
                if (ho->IsArrayBuffer()) {
                    auto* ab = static_cast<JSArrayBuffer*>(ho);
                    size_t byte_offset = 0;
                    if (args.size() > 1 && args[1].IsNumber()) {
                        double bo_d = args[1].AsNumber();
                        if (bo_d > 0) byte_offset = static_cast<size_t>(bo_d);
                    }
                    size_t length = 0;
                    if (args.size() > 2 && args[2].IsNumber()) {
                        double l_d = args[2].AsNumber();
                        if (l_d > 0) length = static_cast<size_t>(l_d);
                    } else {
                        if (ab->ByteLength() >= byte_offset) {
                            length = (ab->ByteLength() - byte_offset) / elem_size;
                        }
                    }
                    auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, byte_offset, length, root_shape);
                    ta->SetPrototype(proto);
                    return TaggedValue::FromHeapObject(ta);
                }

                if (ho->IsTypedArray()) {
                    auto* src_ta = static_cast<JSTypedArray*>(ho);
                    size_t len = src_ta->Length();
                    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
                    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                    }
                    auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, len, root_shape);
                    ta->SetPrototype(proto);
                    for (size_t i = 0; i < len; ++i) {
                        ta->SetElement(i, src_ta->GetElement(i));
                    }
                    return TaggedValue::FromHeapObject(ta);
                }

                TaggedValue iter_fn = GetProperty(first, "@@iterator");
                if (iter_fn.IsHeapObject() && (iter_fn.AsHeapObject()->IsFunction() || iter_fn.AsHeapObject()->IsNativeFunction())) {
                    std::vector<TaggedValue> items;
                    TaggedValue iter = CallFunction(iter_fn, first, {});
                    if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
                    while (true) {
                        TaggedValue next_res = CallFunction(GetProperty(iter, "next"), iter, {});
                        if (Interpreter::Current() && Interpreter::Current()->HasPendingException()) return TaggedValue::Undefined();
                        if (GetProperty(next_res, "done").ToBoolean()) break;
                        items.push_back(GetProperty(next_res, "value"));
                    }
                    size_t len = items.size();
                    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
                    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                    }
                    auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, len, root_shape);
                    ta->SetPrototype(proto);
                    for (size_t i = 0; i < len; ++i) {
                        ta->SetElement(i, items[i]);
                    }
                    return TaggedValue::FromHeapObject(ta);
                }

                if (ho->IsJSObject()) {
                    auto* jso = static_cast<JSObject*>(ho);
                    uint64_t len = ToLength(jso->GetNamedProperty("length"));
                    auto* ab = AllocateRuntime<JSArrayBuffer>(static_cast<size_t>(len) * elem_size, root_shape);
                    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                    }
                    auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, static_cast<size_t>(len), root_shape);
                    ta->SetPrototype(proto);
                    for (size_t i = 0; i < static_cast<size_t>(len); ++i) {
                        ta->SetElement(i, GetProperty(first, std::to_string(i)));
                    }
                    return TaggedValue::FromHeapObject(ta);
                }
            }

            auto* ab = AllocateRuntime<JSArrayBuffer>(0, root_shape);
            auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, 0, root_shape);
            ta->SetPrototype(proto);
            return TaggedValue::FromHeapObject(ta);
        });

        ctor->SetIsConstructor(true);
        ctor->SetNamedProperty("length", TaggedValue::FromSmi(3), next_shape_id_);
        ctor->SetNamedProperty("BYTES_PER_ELEMENT", TaggedValue::FromSmi(static_cast<int32_t>(elem_size)), next_shape_id_);
        ctor->SetOwnPropertyAttributes("BYTES_PER_ELEMENT", PropertyAttributes::None);
        ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(proto), next_shape_id_);
        ctor->SetOwnPropertyAttributes("prototype", PropertyAttributes::None);
        ctor->SetPrototype(typed_array_ctor);
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(ctor), next_shape_id_);
        proto->SetOwnPropertyAttributes("constructor", PropertyAttributes::Writable | PropertyAttributes::Configurable);
        global_object_->SetNamedProperty(name, TaggedValue::FromHeapObject(ctor), next_shape_id_);
        return ctor;
    };

    make_typed_array_ctor(TypedArrayKind::Int8, "Int8Array");
    make_typed_array_ctor(TypedArrayKind::Uint8, "Uint8Array");
    make_typed_array_ctor(TypedArrayKind::Uint8Clamped, "Uint8ClampedArray");
    make_typed_array_ctor(TypedArrayKind::Int16, "Int16Array");
    make_typed_array_ctor(TypedArrayKind::Uint16, "Uint16Array");
    make_typed_array_ctor(TypedArrayKind::Int32, "Int32Array");
    make_typed_array_ctor(TypedArrayKind::Uint32, "Uint32Array");
    make_typed_array_ctor(TypedArrayKind::Float32, "Float32Array");
    make_typed_array_ctor(TypedArrayKind::Float64, "Float64Array");
    make_typed_array_ctor(TypedArrayKind::BigInt64, "BigInt64Array");
    make_typed_array_ctor(TypedArrayKind::BigUint64, "BigUint64Array");

    // $262 host object
    auto* host262 = make_old_obj();
    host262->SetNamedProperty("global", TaggedValue::FromHeapObject(global_object_), next_shape_id_);
    host262->SetNamedProperty("evalScript", TaggedValue::FromHeapObject(make_native("evalScript", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsString()) {
            return TaggedValue::Undefined();
        }
        auto* s = static_cast<JSString*>(args[0].AsHeapObject());
        if (Agent::Current()) {
            return Agent::Current()->EvaluateScript(s->Value());
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);
    host262->SetNamedProperty("gc", TaggedValue::FromHeapObject(make_native("gc", [](const std::vector<TaggedValue>&) -> TaggedValue {
        if (Agent::Current()) {
            Agent::Current()->GetHeap().MinorGC();
            Agent::Current()->GetHeap().MajorGC();
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);
    host262->SetNamedProperty("createRealm", TaggedValue::FromHeapObject(make_native("createRealm", [](const std::vector<TaggedValue>&) -> TaggedValue {
        if (Agent::Current()) {
            auto* new_realm = new Realm(Agent::Current()->GetHeap());
            return new_realm->GetGlobalProperty("$262");
        }
        return TaggedValue::Undefined();
    })), next_shape_id_);

    // $262.agent
    auto* host262_agent = make_old_obj();
    host262_agent->SetNamedProperty("monotonic", TaggedValue::FromHeapObject(make_native("monotonic", [](const std::vector<TaggedValue>&) -> TaggedValue {
        if (EventLoop::Current()) {
            return TaggedValue::FromDouble(EventLoop::Current()->PerformanceNow());
        }
        return TaggedValue::FromDouble(0.0);
    })), next_shape_id_);
    host262->SetNamedProperty("agent", TaggedValue::FromHeapObject(host262_agent), next_shape_id_);
    InitializeBoolean(*this);
    InitializeBigInt(*this);
    InitializeNumber(*this);
    InitializeDate(*this);
    InitializeIntl(*this);
    InitializeWebAssembly(*this);
    InitializeAdvancedIntrinsics(*this);

    global_object_->SetNamedProperty("$262", TaggedValue::FromHeapObject(host262), next_shape_id_);
}

TaggedValue Realm::GetGlobalProperty(std::string_view name) {
    return global_object_->GetNamedProperty(name);
}

void Realm::SetGlobalProperty(std::string_view name, TaggedValue value) {
    global_object_->SetNamedProperty(name, value, next_shape_id_);
}

Agent::Agent() {
    s_current_agent_ = this;
    heap_ = std::make_unique<Heap>();
    NoGCScope no_gc(*heap_);
    IsolateHeap::SetCurrent(heap_.get());
    realm_ = std::make_unique<Realm>(*heap_);
    Realm::SetCurrent(realm_.get());
    s_current_microtasks_ = &microtasks_;
    Interpreter::SetCurrent(&interpreter_);
    event_loop_ = std::make_unique<EventLoop>();
    EventLoop::SetCurrent(event_loop_.get());
    module_loader_ = std::make_unique<ModuleLoader>(*this);
    ModuleLoader::SetCurrent(module_loader_.get());
    module_loader_->InitializeModuleGlobals();
}

Agent::~Agent() {
    AsyncWaitManager::Instance().CancelAndJoinForAgent(this);
    if (s_current_agent_ == this) {
        s_current_agent_ = nullptr;
    }
    if (IsolateHeap::Current() == heap_.get()) {
        IsolateHeap::SetCurrent(nullptr);
    }
    if (Realm::Current() == realm_.get()) {
        Realm::SetCurrent(nullptr);
    }
    if (Interpreter::Current() == &interpreter_) {
        Interpreter::SetCurrent(nullptr);
    }
    if (EventLoop::Current() == event_loop_.get()) {
        EventLoop::SetCurrent(nullptr);
    }
    if (ModuleLoader::Current() == module_loader_.get()) {
        ModuleLoader::SetCurrent(nullptr);
    }
    if (s_current_microtasks_ == &microtasks_) {
        s_current_microtasks_ = nullptr;
    }
    module_loader_.reset();
    event_loop_.reset();
    realm_.reset();
    interpreter_.UnregisterRoots();
    microtasks_.Teardown();
    heap_.reset();
}

void Agent::RunEventLoop() {
    s_current_agent_ = this;
    IsolateHeap::SetCurrent(heap_.get());
    s_current_microtasks_ = &microtasks_;
    Realm::SetCurrent(realm_.get());
    Interpreter::SetCurrent(&interpreter_);
    EventLoop::SetCurrent(event_loop_.get());
    ModuleLoader::SetCurrent(module_loader_.get());
    event_loop_->Run(*this);
}

TaggedValue Agent::EvaluateScriptWithEventLoop(std::string_view source) {
    s_current_agent_ = this;
    IsolateHeap::SetCurrent(heap_.get());
    s_current_microtasks_ = &microtasks_;
    Realm::SetCurrent(realm_.get());
    Interpreter::SetCurrent(&interpreter_);
    EventLoop::SetCurrent(event_loop_.get());
    ModuleLoader::SetCurrent(module_loader_.get());

    Lexer lexer(source);
    Parser parser(lexer);
    auto ast = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bc = emitter.Emit(ast.get());

    TaggedValue global_this = realm_ && realm_->GetGlobalObject() ? TaggedValue::FromHeapObject(realm_->GetGlobalObject()) : TaggedValue::Undefined();
    TaggedValue result = interpreter_.Execute(*bc, {}, global_this);
    LexicalEnvironment* script_env = interpreter_.GetLastEnvironment();

    // Initial microtask checkpoint
    microtasks_.PerformMicrotaskCheckpoint();

    // Run the event loop to completion
    event_loop_->Run(*this);

    // Final check for trailing context slot expression
    if (ast && !ast->Statements().empty()) {
        auto* last_stmt = ast->Statements().back().get();
        if (auto* expr_stmt = dynamic_cast<ExprStmt*>(last_stmt)) {
            if (auto* id_expr = dynamic_cast<IdentifierExpr*>(expr_stmt->GetExpr())) {
                auto res = emitter.ResolveVariable(id_expr->Name());
                if (res.kind == BytecodeEmitter::VarResolution::ContextSlot && script_env) {
                    result = script_env->Get(res.depth, res.slot);
                }
            }
        }
    }

    return result;
}

TaggedValue Agent::EvaluateScript(std::string_view source, TaggedValue this_val) {
    s_current_agent_ = this;
    IsolateHeap::SetCurrent(heap_.get());
    s_current_microtasks_ = &microtasks_;
    Realm::SetCurrent(realm_.get());
    Interpreter::SetCurrent(&interpreter_);
    EventLoop::SetCurrent(event_loop_.get());
    ModuleLoader::SetCurrent(module_loader_.get());

    Lexer lexer(source);
    Parser parser(lexer);
    auto ast = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bc = emitter.Emit(ast.get());

    TaggedValue effective_this = this_val;
    if (effective_this.IsUndefined() && realm_ && realm_->GetGlobalObject()) {
        effective_this = TaggedValue::FromHeapObject(realm_->GetGlobalObject());
    }
    TaggedValue result = interpreter_.Execute(*bc, {}, effective_this);
    LexicalEnvironment* script_env = interpreter_.GetLastEnvironment();

    // Drain microtasks at script execution checkpoint
    bool had_microtasks = !microtasks_.Empty();
    Interpreter::SetCurrent(&interpreter_);
    microtasks_.PerformMicrotaskCheckpoint();

    if (had_microtasks && ast && !ast->Statements().empty()) {
        auto* last_stmt = ast->Statements().back().get();
        if (auto* expr_stmt = dynamic_cast<ExprStmt*>(last_stmt)) {
            if (auto* id_expr = dynamic_cast<IdentifierExpr*>(expr_stmt->GetExpr())) {
                auto res = emitter.ResolveVariable(id_expr->Name());
                if (res.kind == BytecodeEmitter::VarResolution::ContextSlot && script_env) {
                    result = script_env->Get(res.depth, res.slot);
                }
            }
        }
    }

    return result;
}

TaggedValue Agent::EvaluateModule(std::string_view source, std::string_view specifier) {
    s_current_agent_ = this;
    IsolateHeap::SetCurrent(heap_.get());
    s_current_microtasks_ = &microtasks_;
    Realm::SetCurrent(realm_.get());
    Interpreter::SetCurrent(&interpreter_);
    EventLoop::SetCurrent(event_loop_.get());
    ModuleLoader::SetCurrent(module_loader_.get());

    module_loader_->RegisterVirtualModule(specifier, source);
    auto mod = module_loader_->LoadModule(specifier);
    module_loader_->LinkModule(mod.get());
    TaggedValue result = module_loader_->EvaluateModule(mod.get());
    microtasks_.PerformMicrotaskCheckpoint();
    return result;
}

TaggedValue Agent::EvaluateModuleWithEventLoop(std::string_view source, std::string_view specifier) {
    s_current_agent_ = this;
    IsolateHeap::SetCurrent(heap_.get());
    s_current_microtasks_ = &microtasks_;
    Realm::SetCurrent(realm_.get());
    Interpreter::SetCurrent(&interpreter_);
    EventLoop::SetCurrent(event_loop_.get());
    ModuleLoader::SetCurrent(module_loader_.get());

    module_loader_->RegisterVirtualModule(specifier, source);
    auto mod = module_loader_->LoadModule(specifier);
    module_loader_->LinkModule(mod.get());
    TaggedValue result = module_loader_->EvaluateModule(mod.get());
    microtasks_.PerformMicrotaskCheckpoint();
    event_loop_->Run(*this);
    return result;
}

} // namespace aether
