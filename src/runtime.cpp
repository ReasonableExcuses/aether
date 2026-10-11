#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include "aether/typed_array.h"
#include <cmath>
#include <iostream>
#include <sstream>
#include <algorithm>
#include <cctype>

namespace aether {

static thread_local MicrotaskQueue* s_current_microtasks_{nullptr};
static thread_local Agent* s_current_agent_{nullptr};

Agent* Agent::Current() noexcept { return s_current_agent_; }
void Agent::SetCurrent(Agent* agent) noexcept { s_current_agent_ = agent; }

static MicrotaskQueue& CurrentMicrotaskQueue() {
    static MicrotaskQueue s_fallback_queue;
    if (s_current_microtasks_) {
        return *s_current_microtasks_;
    }
    return s_fallback_queue;
}

TaggedValue CallFunction(TaggedValue callee, TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!callee.IsHeapObject()) return TaggedValue::Undefined();
    HeapObject* h_obj = callee.AsHeapObject();
    if (h_obj->IsNativeFunction()) {
        auto* native_fn = static_cast<JSNativeFunction*>(h_obj);
        return native_fn->Invoke(this_val, args);
    }
    if (h_obj->IsFunction()) {
        auto* fn = static_cast<JSFunction*>(h_obj);
        if (fn->IsGenerator()) {
            Shape* root = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
            auto* gen_obj = AllocateRuntime<JSGeneratorObject>(fn, args, this_val, fn->GetEnvironment(), root);
            if (Realm::Current() && Realm::Current()->GetGeneratorPrototype()) {
                gen_obj->SetPrototype(Realm::Current()->GetGeneratorPrototype());
            }
            return TaggedValue::FromHeapObject(gen_obj);
        }
        if (Interpreter::Current()) {
            return Interpreter::Current()->Execute(*fn->GetBytecode(), args, this_val, fn->GetEnvironment());
        }
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
    if (args.size() < 2) return TaggedValue::FromDouble(std::nan(""));
    double base = args[0].AsNumber();
    double exp = args[1].AsNumber();
    return TaggedValue::FromDouble(std::pow(base, exp));
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

// Array Prototype Methods
static TaggedValue Array_push(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::FromSmi(0);
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    for (const auto& a : args) {
        arr->Push(a);
    }
    return TaggedValue::FromSmi(static_cast<int32_t>(arr->Length()));
}

static TaggedValue Array_pop(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    return arr->Pop();
}

static TaggedValue Array_shift(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    if (arr->Length() == 0) return TaggedValue::Undefined();
    TaggedValue first = arr->GetElement(0);
    arr->Elements().erase(arr->Elements().begin());
    return first;
}

static TaggedValue Array_unshift(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::FromSmi(0);
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    arr->Elements().insert(arr->Elements().begin(), args.begin(), args.end());
    return TaggedValue::FromSmi(static_cast<int32_t>(arr->Length()));
}

static TaggedValue Array_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(arr->Length());
    int64_t start = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;

    if (start < 0) start = std::max<int64_t>(0, len + start);
    else start = std::min<int64_t>(len, start);

    if (end < 0) end = std::max<int64_t>(0, len + end);
    else end = std::min<int64_t>(len, end);

    std::vector<TaggedValue> sliced;
    if (start < end) {
        sliced.reserve(static_cast<size_t>(end - start));
        for (int64_t i = start; i < end; ++i) {
            sliced.push_back(arr->GetElement(static_cast<size_t>(i)));
        }
    }
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, std::move(sliced));
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_join(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    std::string sep = args.size() > 0 ? args[0].ToString() : ",";
    std::string res;
    for (size_t i = 0; i < arr->Length(); ++i) {
        if (i > 0) res += sep;
        TaggedValue el = arr->GetElement(i);
        if (!el.IsUndefined() && !el.IsNull()) {
            res += el.ToString();
        }
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

static TaggedValue Array_indexOf(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::FromSmi(-1);
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue search = args.empty() ? TaggedValue::Undefined() : args[0];
    size_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<size_t>(std::max<int32_t>(0, args[1].AsSmi())) : 0;
    for (size_t i = from; i < arr->Length(); ++i) {
        if (arr->GetElement(i) == search) {
            return TaggedValue::FromSmi(static_cast<int32_t>(i));
        }
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue Array_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::FromBoolean(false);
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue search = args.empty() ? TaggedValue::Undefined() : args[0];
    for (size_t i = 0; i < arr->Length(); ++i) {
        if (arr->GetElement(i) == search) {
            return TaggedValue::FromBoolean(true);
        }
    }
    return TaggedValue::FromBoolean(false);
}

static TaggedValue Array_reverse(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return this_val;
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    std::reverse(arr->Elements().begin(), arr->Elements().end());
    return this_val;
}

static TaggedValue Array_concat(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (size_t i = 0; i < arr->Length(); ++i) {
        new_arr->Push(arr->GetElement(i));
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
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue cb = args.empty() ? TaggedValue::Undefined() : args[0];
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, arr->Length());
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (size_t i = 0; i < arr->Length(); ++i) {
        TaggedValue res = CallFunction(cb, this_val, {arr->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
        new_arr->SetElement(i, res);
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_filter(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue cb = args.empty() ? TaggedValue::Undefined() : args[0];
    auto* new_arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        new_arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }
    for (size_t i = 0; i < arr->Length(); ++i) {
        TaggedValue el = arr->GetElement(i);
        TaggedValue res = CallFunction(cb, this_val, {el, TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
        if (res.ToBoolean()) {
            new_arr->Push(el);
        }
    }
    return TaggedValue::FromHeapObject(new_arr);
}

static TaggedValue Array_forEach(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue cb = args.empty() ? TaggedValue::Undefined() : args[0];
    for (size_t i = 0; i < arr->Length(); ++i) {
        CallFunction(cb, this_val, {arr->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
    }
    return TaggedValue::Undefined();
}

static TaggedValue Array_reduce(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsArray()) return TaggedValue::Undefined();
    auto* arr = static_cast<JSArray*>(this_val.AsHeapObject());
    TaggedValue cb = args.empty() ? TaggedValue::Undefined() : args[0];
    size_t start_idx = 0;
    TaggedValue acc;
    if (args.size() > 1) {
        acc = args[1];
    } else {
        if (arr->Length() == 0) return TaggedValue::Undefined();
        acc = arr->GetElement(0);
        start_idx = 1;
    }
    for (size_t i = start_idx; i < arr->Length(); ++i) {
        acc = CallFunction(cb, this_val, {acc, arr->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
    }
    return acc;
}

static TaggedValue Array_isArray(const std::vector<TaggedValue>& args) {
    if (args.empty()) return TaggedValue::FromBoolean(false);
    return TaggedValue::FromBoolean(args[0].IsHeapObject() && args[0].AsHeapObject()->IsArray());
}

// String Prototype Methods
static TaggedValue String_charAt(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    int32_t idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int32_t>(args[0].AsNumber()));
    if (idx >= 0 && static_cast<size_t>(idx) < s->Length()) {
        return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s->View().substr(idx, 1)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
}

static TaggedValue String_charCodeAt(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromDouble(std::nan(""));
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    int32_t idx = args.empty() ? 0 : (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int32_t>(args[0].AsNumber()));
    if (idx >= 0 && static_cast<size_t>(idx) < s->Length()) {
        return TaggedValue::FromSmi(static_cast<uint8_t>(s->View()[idx]));
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
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromSmi(-1);
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    size_t from = (args.size() > 1 && args[1].IsSmi()) ? static_cast<size_t>(std::max<int32_t>(0, args[1].AsSmi())) : 0;
    size_t pos = s->View().find(search, from);
    return TaggedValue::FromSmi(pos != std::string_view::npos ? static_cast<int32_t>(pos) : -1);
}

static TaggedValue String_slice(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(s->Length());
    int64_t start = args.size() > 0 ? (args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? (args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;

    if (start < 0) start = std::max<int64_t>(0, len + start);
    else start = std::min<int64_t>(len, start);

    if (end < 0) end = std::max<int64_t>(0, len + end);
    else end = std::min<int64_t>(len, end);

    if (start >= end) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s->View().substr(start, end - start)));
}

static TaggedValue String_substring(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(""));
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    int64_t len = static_cast<int64_t>(s->Length());
    int64_t start = args.size() > 0 ? std::max<int64_t>(0, args[0].IsSmi() ? args[0].AsSmi() : static_cast<int64_t>(args[0].AsNumber())) : 0;
    int64_t end = args.size() > 1 ? std::max<int64_t>(0, args[1].IsSmi() ? args[1].AsSmi() : static_cast<int64_t>(args[1].AsNumber())) : len;

    start = std::min<int64_t>(len, start);
    end = std::min<int64_t>(len, end);
    if (start > end) std::swap(start, end);

    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s->View().substr(start, end - start)));
}

static TaggedValue String_toLowerCase(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string s = this_val.ToString();
    for (char& c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
}

static TaggedValue String_toUpperCase(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string s = this_val.ToString();
    for (char& c : s) {
        c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s));
}

static TaggedValue String_trim(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    (void)args;
    std::string_view s = static_cast<JSString*>(this_val.AsHeapObject())->View();
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) ++first;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) --last;
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(s.substr(first, last - first)));
}

static TaggedValue String_split(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::Undefined();
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    auto* arr = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        arr->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    if (args.empty()) {
        arr->Push(this_val);
        return TaggedValue::FromHeapObject(arr);
    }

    std::string sep = args[0].ToString();
    std::string_view view = s->View();
    if (sep.empty()) {
        for (char c : view) {
            arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(std::string(1, c))));
        }
    } else {
        size_t start = 0;
        size_t pos = 0;
        while ((pos = view.find(sep, start)) != std::string_view::npos) {
            arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(view.substr(start, pos - start))));
            start = pos + sep.length();
        }
        arr->Push(TaggedValue::FromHeapObject(AllocateRuntime<JSString>(view.substr(start))));
    }
    return TaggedValue::FromHeapObject(arr);
}

static TaggedValue String_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsString()) return TaggedValue::FromBoolean(false);
    auto* s = static_cast<JSString*>(this_val.AsHeapObject());
    std::string search = args.empty() ? "undefined" : args[0].ToString();
    return TaggedValue::FromBoolean(s->View().find(search) != std::string_view::npos);
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

// Map Intrinsics
static TaggedValue Map_ctor(const std::vector<TaggedValue>& args) {
    (void)args;
    auto* m = AllocateRuntime<JSMap>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetMapPrototype()) {
        m->SetPrototype(Realm::Current()->GetMapPrototype());
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
    (void)args;
    auto* s = AllocateRuntime<JSSet>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
    if (Realm::Current() && Realm::Current()->GetSetPrototype()) {
        s->SetPrototype(Realm::Current()->GetSetPrototype());
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

    if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsArray()) {
        p->Fulfill(TaggedValue::FromHeapObject(AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    auto* input_arr = static_cast<JSArray*>(args[0].AsHeapObject());
    size_t count = input_arr->Length();
    if (count == 0) {
        p->Fulfill(TaggedValue::FromHeapObject(AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)), CurrentMicrotaskQueue());
        return TaggedValue::FromHeapObject(p);
    }

    auto* results = AllocateRuntime<JSArray>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr, count);
    if (Realm::Current() && Realm::Current()->GetArrayPrototype()) {
        results->SetPrototype(Realm::Current()->GetArrayPrototype());
    }

    auto remaining = std::make_shared<size_t>(count);
    for (size_t i = 0; i < count; ++i) {
        TaggedValue item = input_arr->GetElement(i);
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

// RegExp Intrinsics
static TaggedValue RegExp_ctor(const std::vector<TaggedValue>& args) {
    std::string pattern = args.size() > 0 ? args[0].ToString() : "";
    std::string flags = args.size() > 1 ? args[1].ToString() : "";
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
        });
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
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::FromSmi(-1);
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue target = args[0];
    size_t from = 0;
    if (args.size() > 1 && args[1].IsNumber()) {
        double d = args[1].AsNumber();
        if (d > 0) from = static_cast<size_t>(d);
    }
    for (size_t i = from; i < ta->Length(); ++i) {
        TaggedValue elem = ta->GetElement(i);
        if (elem.IsNumber() && target.IsNumber() && elem.AsNumber() == target.AsNumber()) {
            return TaggedValue::FromSmi(static_cast<int32_t>(i));
        }
    }
    return TaggedValue::FromSmi(-1);
}

static TaggedValue TypedArray_includes(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    TaggedValue idx = TypedArray_indexOf(this_val, args);
    return TaggedValue::FromBoolean(idx.AsSmi() >= 0);
}

static TaggedValue TypedArray_reverse(TaggedValue this_val, const std::vector<TaggedValue>&) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) return this_val;
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
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray()) return TaggedValue::Undefined();
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    std::string sep = args.size() > 0 ? args[0].ToString() : ",";
    std::string res;
    for (size_t i = 0; i < ta->Length(); ++i) {
        if (i > 0) res += sep;
        res += ta->GetElement(i).ToString();
    }
    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(res));
}

static TaggedValue TypedArray_forEach(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    for (size_t i = 0; i < ta->Length(); ++i) {
        CallFunction(cb, this_val, {ta->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
    }
    return TaggedValue::Undefined();
}

static TaggedValue TypedArray_map(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
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
        TaggedValue res = CallFunction(cb, this_val, {ta->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
        new_ta->SetElement(i, res);
    }
    return TaggedValue::FromHeapObject(new_ta);
}

static TaggedValue TypedArray_filter(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    TaggedValue cb = args[0];
    std::vector<TaggedValue> kept;
    for (size_t i = 0; i < ta->Length(); ++i) {
        TaggedValue el = ta->GetElement(i);
        TaggedValue res = CallFunction(cb, this_val, {el, TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
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
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsTypedArray() || args.empty()) {
        return TaggedValue::Undefined();
    }
    auto* ta = static_cast<JSTypedArray*>(this_val.AsHeapObject());
    if (ta->Length() == 0 && args.size() < 2) return TaggedValue::Undefined();
    TaggedValue cb = args[0];
    size_t i = 0;
    TaggedValue accum;
    if (args.size() > 1) {
        accum = args[1];
    } else {
        accum = ta->GetElement(0);
        i = 1;
    }
    for (; i < ta->Length(); ++i) {
        accum = CallFunction(cb, this_val, {accum, ta->GetElement(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), this_val});
    }
    return accum;
}

// DataView Methods
static TaggedValue DataView_getInt8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetInt8(offset)));
}
static TaggedValue DataView_setInt8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    int8_t val = args[1].IsNumber() ? static_cast<int8_t>(args[1].AsNumber()) : 0;
    dv->SetInt8(offset, val);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetUint8(offset)));
}
static TaggedValue DataView_setUint8(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    uint8_t val = args[1].IsNumber() ? static_cast<uint8_t>(args[1].AsNumber()) : 0;
    dv->SetUint8(offset, val);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getInt16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetInt16(offset, le)));
}
static TaggedValue DataView_setInt16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    int16_t val = args[1].IsNumber() ? static_cast<int16_t>(args[1].AsNumber()) : 0;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetInt16(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    return TaggedValue::FromSmi(static_cast<int32_t>(dv->GetUint16(offset, le)));
}
static TaggedValue DataView_setUint16(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    uint16_t val = args[1].IsNumber() ? static_cast<uint16_t>(args[1].AsNumber()) : 0;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetUint16(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getInt32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    return TaggedValue::FromSmi(dv->GetInt32(offset, le));
}
static TaggedValue DataView_setInt32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    int32_t val = args[1].IsNumber() ? static_cast<int32_t>(args[1].AsNumber()) : 0;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetInt32(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getUint32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    uint32_t u = dv->GetUint32(offset, le);
    if (u <= 0x3FFFFFFF) return TaggedValue::FromSmi(static_cast<int32_t>(u));
    return TaggedValue::FromDouble(static_cast<double>(u));
}
static TaggedValue DataView_setUint32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    uint32_t val = args[1].IsNumber() ? static_cast<uint32_t>(args[1].AsNumber()) : 0;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetUint32(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getFloat32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    return TaggedValue::FromDouble(static_cast<double>(dv->GetFloat32(offset, le)));
}
static TaggedValue DataView_setFloat32(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    float val = args[1].IsNumber() ? static_cast<float>(args[1].AsNumber()) : 0.0f;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetFloat32(offset, val, le);
    return TaggedValue::Undefined();
}

static TaggedValue DataView_getFloat64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.empty()) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    bool le = args.size() > 1 && args[1].ToBoolean();
    return TaggedValue::FromDouble(dv->GetFloat64(offset, le));
}
static TaggedValue DataView_setFloat64(TaggedValue this_val, const std::vector<TaggedValue>& args) {
    if (!this_val.IsHeapObject() || !this_val.AsHeapObject()->IsDataView() || args.size() < 2) return TaggedValue::Undefined();
    auto* dv = static_cast<JSDataView*>(this_val.AsHeapObject());
    size_t offset = args[0].IsNumber() ? static_cast<size_t>(args[0].AsNumber()) : 0;
    double val = args[1].IsNumber() ? args[1].AsNumber() : 0.0;
    bool le = args.size() > 2 && args[2].ToBoolean();
    dv->SetFloat64(offset, val, le);
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
    heap_ = &heap;
    s_current_realm_ = this;

    auto make_old_obj = [this, &heap]() -> JSObject* {
        JSObject* obj = heap.AllocateOld<JSObject>(root_shape_.get());
        auto val = std::make_unique<TaggedValue>(TaggedValue::FromHeapObject(obj));
        heap.RegisterExtraRoot(val.get());
        root_values_.push_back(std::move(val));
        return obj;
    };

    auto make_native = [this, &heap](std::string_view name, auto cb) -> JSNativeFunction* {
        JSNativeFunction* fn = heap.AllocateOld<JSNativeFunction>(name, cb);
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

    global_object_ = make_old_obj();
    math_object_ = make_old_obj();
    console_object_ = make_old_obj();
    json_object_ = make_old_obj();

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

    // Well-Known Symbols
    static uint32_t s_symbol_counter = 1000;
    auto make_symbol = [&heap](std::string_view desc) -> JSSymbol* {
        uint32_t id = ++s_symbol_counter;
        JSSymbol* sym = heap.AllocateOld<JSSymbol>(desc, id);
        return sym;
    };

    auto* sym_iterator = make_symbol("@@iterator");
    auto* sym_toStringTag = make_symbol("@@toStringTag");
    auto* sym_hasInstance = make_symbol("@@hasInstance");
    auto* sym_toPrimitive = make_symbol("@@toPrimitive");

    symbol_iterator_ = TaggedValue::FromHeapObject(sym_iterator);
    symbol_toStringTag_ = TaggedValue::FromHeapObject(sym_toStringTag);
    symbol_hasInstance_ = TaggedValue::FromHeapObject(sym_hasInstance);
    symbol_toPrimitive_ = TaggedValue::FromHeapObject(sym_toPrimitive);

    auto sym_iter_root = std::make_unique<TaggedValue>(symbol_iterator_);
    heap.RegisterExtraRoot(sym_iter_root.get());
    root_values_.push_back(std::move(sym_iter_root));

    auto sym_tag_root = std::make_unique<TaggedValue>(symbol_toStringTag_);
    heap.RegisterExtraRoot(sym_tag_root.get());
    root_values_.push_back(std::move(sym_tag_root));

    auto sym_inst_root = std::make_unique<TaggedValue>(symbol_hasInstance_);
    heap.RegisterExtraRoot(sym_inst_root.get());
    root_values_.push_back(std::move(sym_inst_root));

    auto sym_prim_root = std::make_unique<TaggedValue>(symbol_toPrimitive_);
    heap.RegisterExtraRoot(sym_prim_root.get());
    root_values_.push_back(std::move(sym_prim_root));

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

    auto* gen_iter_self = make_native("[Symbol.iterator]", [](TaggedValue this_val, const std::vector<TaggedValue>&) -> TaggedValue {
        return this_val;
    });
    generator_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(gen_iter_self), next_shape_id_);

    // Register Math functions
    math_object_->SetNamedProperty("abs", TaggedValue::FromHeapObject(make_native("abs", &Math_abs)), next_shape_id_);
    math_object_->SetNamedProperty("floor", TaggedValue::FromHeapObject(make_native("floor", &Math_floor)), next_shape_id_);
    math_object_->SetNamedProperty("ceil", TaggedValue::FromHeapObject(make_native("ceil", &Math_ceil)), next_shape_id_);
    math_object_->SetNamedProperty("sqrt", TaggedValue::FromHeapObject(make_native("sqrt", &Math_sqrt)), next_shape_id_);
    math_object_->SetNamedProperty("max", TaggedValue::FromHeapObject(make_native("max", &Math_max)), next_shape_id_);
    math_object_->SetNamedProperty("min", TaggedValue::FromHeapObject(make_native("min", &Math_min)), next_shape_id_);
    math_object_->SetNamedProperty("pow", TaggedValue::FromHeapObject(make_native("pow", &Math_pow)), next_shape_id_);

    // Register Console functions
    console_object_->SetNamedProperty("log", TaggedValue::FromHeapObject(make_native("log", &Console_log)), next_shape_id_);

    // Array Prototype Methods
    array_prototype_->SetNamedProperty("push", TaggedValue::FromHeapObject(make_native("push", &Array_push)), next_shape_id_);
    array_prototype_->SetNamedProperty("pop", TaggedValue::FromHeapObject(make_native("pop", &Array_pop)), next_shape_id_);
    array_prototype_->SetNamedProperty("shift", TaggedValue::FromHeapObject(make_native("shift", &Array_shift)), next_shape_id_);
    array_prototype_->SetNamedProperty("unshift", TaggedValue::FromHeapObject(make_native("unshift", &Array_unshift)), next_shape_id_);
    array_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &Array_slice)), next_shape_id_);
    array_prototype_->SetNamedProperty("join", TaggedValue::FromHeapObject(make_native("join", &Array_join)), next_shape_id_);
    array_prototype_->SetNamedProperty("indexOf", TaggedValue::FromHeapObject(make_native("indexOf", &Array_indexOf)), next_shape_id_);
    array_prototype_->SetNamedProperty("includes", TaggedValue::FromHeapObject(make_native("includes", &Array_includes)), next_shape_id_);
    array_prototype_->SetNamedProperty("reverse", TaggedValue::FromHeapObject(make_native("reverse", &Array_reverse)), next_shape_id_);
    array_prototype_->SetNamedProperty("concat", TaggedValue::FromHeapObject(make_native("concat", &Array_concat)), next_shape_id_);
    array_prototype_->SetNamedProperty("map", TaggedValue::FromHeapObject(make_native("map", &Array_map)), next_shape_id_);
    array_prototype_->SetNamedProperty("filter", TaggedValue::FromHeapObject(make_native("filter", &Array_filter)), next_shape_id_);
    array_prototype_->SetNamedProperty("forEach", TaggedValue::FromHeapObject(make_native("forEach", &Array_forEach)), next_shape_id_);
    array_prototype_->SetNamedProperty("reduce", TaggedValue::FromHeapObject(make_native("reduce", &Array_reduce)), next_shape_id_);
    auto* arr_values = make_native("values", &Array_values);
    auto* arr_keys = make_native("keys", &Array_keys);
    auto* arr_entries = make_native("entries", &Array_entries);
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
    });
    array_ctor->SetNamedProperty("isArray", TaggedValue::FromHeapObject(make_native("isArray", &Array_isArray)), next_shape_id_);
    array_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(array_prototype_), next_shape_id_);

    // String Prototype Methods
    string_prototype_->SetNamedProperty("charAt", TaggedValue::FromHeapObject(make_native("charAt", &String_charAt)), next_shape_id_);
    string_prototype_->SetNamedProperty("charCodeAt", TaggedValue::FromHeapObject(make_native("charCodeAt", &String_charCodeAt)), next_shape_id_);
    string_prototype_->SetNamedProperty("concat", TaggedValue::FromHeapObject(make_native("concat", &String_concat)), next_shape_id_);
    string_prototype_->SetNamedProperty("indexOf", TaggedValue::FromHeapObject(make_native("indexOf", &String_indexOf)), next_shape_id_);
    string_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &String_slice)), next_shape_id_);
    string_prototype_->SetNamedProperty("substring", TaggedValue::FromHeapObject(make_native("substring", &String_substring)), next_shape_id_);
    string_prototype_->SetNamedProperty("toLowerCase", TaggedValue::FromHeapObject(make_native("toLowerCase", &String_toLowerCase)), next_shape_id_);
    string_prototype_->SetNamedProperty("toUpperCase", TaggedValue::FromHeapObject(make_native("toUpperCase", &String_toUpperCase)), next_shape_id_);
    string_prototype_->SetNamedProperty("trim", TaggedValue::FromHeapObject(make_native("trim", &String_trim)), next_shape_id_);
    string_prototype_->SetNamedProperty("split", TaggedValue::FromHeapObject(make_native("split", &String_split)), next_shape_id_);
    string_prototype_->SetNamedProperty("includes", TaggedValue::FromHeapObject(make_native("includes", &String_includes)), next_shape_id_);
    auto* str_iter = make_native("values", &String_iterator);
    string_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(str_iter), next_shape_id_);

    // JSON Methods
    json_object_->SetNamedProperty("parse", TaggedValue::FromHeapObject(make_native("parse", &JSON_parse)), next_shape_id_);
    json_object_->SetNamedProperty("stringify", TaggedValue::FromHeapObject(make_native("stringify", &JSON_stringify)), next_shape_id_);

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
    map_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(map_prototype_), next_shape_id_);

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
    set_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(set_prototype_), next_shape_id_);

    // Promise Prototype & Constructor
    promise_prototype_->SetNamedProperty("then", TaggedValue::FromHeapObject(make_native("then", &Promise_then)), next_shape_id_);
    promise_prototype_->SetNamedProperty("catch", TaggedValue::FromHeapObject(make_native("catch", &Promise_catch)), next_shape_id_);
    auto* promise_ctor = make_native("Promise", &Promise_ctor);
    promise_ctor->SetNamedProperty("resolve", TaggedValue::FromHeapObject(make_native("resolve", &Promise_resolve)), next_shape_id_);
    promise_ctor->SetNamedProperty("reject", TaggedValue::FromHeapObject(make_native("reject", &Promise_reject)), next_shape_id_);
    promise_ctor->SetNamedProperty("all", TaggedValue::FromHeapObject(make_native("all", &Promise_all)), next_shape_id_);
    promise_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(promise_prototype_), next_shape_id_);

    // RegExp Prototype & Constructor
    regexp_prototype_->SetNamedProperty("test", TaggedValue::FromHeapObject(make_native("test", &RegExp_test)), next_shape_id_);
    auto* regexp_ctor = make_native("RegExp", &RegExp_ctor);
    regexp_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(regexp_prototype_), next_shape_id_);

    // Attach to Global Object
    global_object_->SetNamedProperty("Math", TaggedValue::FromHeapObject(math_object_), next_shape_id_);
    global_object_->SetNamedProperty("console", TaggedValue::FromHeapObject(console_object_), next_shape_id_);
    global_object_->SetNamedProperty("JSON", TaggedValue::FromHeapObject(json_object_), next_shape_id_);
    global_object_->SetNamedProperty("Array", TaggedValue::FromHeapObject(array_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Map", TaggedValue::FromHeapObject(map_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Set", TaggedValue::FromHeapObject(set_ctor), next_shape_id_);
    global_object_->SetNamedProperty("Promise", TaggedValue::FromHeapObject(promise_ctor), next_shape_id_);
    global_object_->SetNamedProperty("RegExp", TaggedValue::FromHeapObject(regexp_ctor), next_shape_id_);

    // Object Constructor & Prototype
    auto* object_ctor = make_native("Object", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || args[0].IsUndefined() || args[0].IsNull()) {
            return TaggedValue::FromHeapObject(AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr));
        }
        if (args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            return args[0];
        }
        return TaggedValue::FromHeapObject(AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr));
    });
    auto* object_prototype = make_old_obj();
    object_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(object_prototype), next_shape_id_);
    object_ctor->SetNamedProperty("create", TaggedValue::FromHeapObject(make_native("create", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        auto* obj = AllocateRuntime<JSObject>(Realm::Current() ? Realm::Current()->GetRootShape() : nullptr);
        if (!args.empty() && args[0].IsHeapObject() && args[0].AsHeapObject()->IsJSObject()) {
            obj->SetPrototype(static_cast<JSObject*>(args[0].AsHeapObject()));
        }
        return TaggedValue::FromHeapObject(obj);
    })), next_shape_id_);
    global_object_->SetNamedProperty("Object", TaggedValue::FromHeapObject(object_ctor), next_shape_id_);

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
    symbol_ctor->SetNamedProperty("toStringTag", symbol_toStringTag_, next_shape_id_);
    symbol_ctor->SetNamedProperty("hasInstance", symbol_hasInstance_, next_shape_id_);
    symbol_ctor->SetNamedProperty("toPrimitive", symbol_toPrimitive_, next_shape_id_);

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
    auto make_error_ctor = [this, &make_old_obj, &make_native, &make_old_str](std::string_view err_name) -> JSNativeFunction* {
        auto* proto = make_old_obj();
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
        ctor->SetNamedProperty("name", TaggedValue::FromHeapObject(make_old_str(name_str)), next_shape_id_);
        ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(proto), next_shape_id_);
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(ctor), next_shape_id_);
        return ctor;
    };

    global_object_->SetNamedProperty("Error", TaggedValue::FromHeapObject(make_error_ctor("Error")), next_shape_id_);
    global_object_->SetNamedProperty("TypeError", TaggedValue::FromHeapObject(make_error_ctor("TypeError")), next_shape_id_);
    global_object_->SetNamedProperty("RangeError", TaggedValue::FromHeapObject(make_error_ctor("RangeError")), next_shape_id_);
    global_object_->SetNamedProperty("ReferenceError", TaggedValue::FromHeapObject(make_error_ctor("ReferenceError")), next_shape_id_);
    global_object_->SetNamedProperty("SyntaxError", TaggedValue::FromHeapObject(make_error_ctor("SyntaxError")), next_shape_id_);
    global_object_->SetNamedProperty("Test262Error", TaggedValue::FromHeapObject(make_error_ctor("Test262Error")), next_shape_id_);

    // isNaN and isFinite
    global_object_->SetNamedProperty("isNaN", TaggedValue::FromHeapObject(make_native("isNaN", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::FromBoolean(true);
        if (args[0].IsSmi()) return TaggedValue::FromBoolean(false);
        if (args[0].IsDouble()) return TaggedValue::FromBoolean(std::isnan(args[0].AsDouble()));
        return TaggedValue::FromBoolean(true);
    })), next_shape_id_);

    global_object_->SetNamedProperty("isFinite", TaggedValue::FromHeapObject(make_native("isFinite", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty()) return TaggedValue::FromBoolean(false);
        if (args[0].IsSmi()) return TaggedValue::FromBoolean(true);
        if (args[0].IsDouble()) return TaggedValue::FromBoolean(std::isfinite(args[0].AsDouble()));
        return TaggedValue::FromBoolean(false);
    })), next_shape_id_);

    // ArrayBuffer Prototype & Constructor
    array_buffer_prototype_->SetNamedProperty("slice", TaggedValue::FromHeapObject(make_native("slice", &ArrayBuffer_slice)), next_shape_id_);
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
    array_buffer_ctor->SetNamedProperty("isView", TaggedValue::FromHeapObject(make_native("isView", &ArrayBuffer_isView)), next_shape_id_);
    array_buffer_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(array_buffer_prototype_), next_shape_id_);
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

    auto* data_view_ctor = make_native("DataView", [](const std::vector<TaggedValue>& args) -> TaggedValue {
        if (args.empty() || !args[0].IsHeapObject() || !args[0].AsHeapObject()->IsArrayBuffer()) {
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
    auto* ta_values = make_native("values", &TypedArray_values);
    auto* ta_keys = make_native("keys", &TypedArray_keys);
    auto* ta_entries = make_native("entries", &TypedArray_entries);
    typed_array_prototype_->SetNamedProperty("values", TaggedValue::FromHeapObject(ta_values), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("keys", TaggedValue::FromHeapObject(ta_keys), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("entries", TaggedValue::FromHeapObject(ta_entries), next_shape_id_);
    typed_array_prototype_->SetNamedProperty("@@iterator", TaggedValue::FromHeapObject(ta_values), next_shape_id_);

    auto make_typed_array_ctor = [&](TypedArrayKind kind, std::string_view name) -> JSNativeFunction* {
        auto* proto = make_old_obj();
        proto->SetPrototype(typed_array_prototype_);
        size_t elem_size = TypedArrayElementSize(kind);
        proto->SetNamedProperty("BYTES_PER_ELEMENT", TaggedValue::FromSmi(static_cast<int32_t>(elem_size)), next_shape_id_);

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

                if (ho->IsArray()) {
                    auto* src_arr = static_cast<JSArray*>(ho);
                    size_t len = src_arr->Length();
                    auto* ab = AllocateRuntime<JSArrayBuffer>(len * elem_size, root_shape);
                    if (Realm::Current() && Realm::Current()->GetArrayBufferPrototype()) {
                        ab->SetPrototype(Realm::Current()->GetArrayBufferPrototype());
                    }
                    auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, len, root_shape);
                    ta->SetPrototype(proto);
                    for (size_t i = 0; i < len; ++i) {
                        ta->SetElement(i, src_arr->GetElement(i));
                    }
                    return TaggedValue::FromHeapObject(ta);
                }
            }

            auto* ab = AllocateRuntime<JSArrayBuffer>(0, root_shape);
            auto* ta = AllocateRuntime<JSTypedArray>(kind, ab, 0, 0, root_shape);
            ta->SetPrototype(proto);
            return TaggedValue::FromHeapObject(ta);
        });

        ctor->SetNamedProperty("BYTES_PER_ELEMENT", TaggedValue::FromSmi(static_cast<int32_t>(elem_size)), next_shape_id_);
        ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(proto), next_shape_id_);
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
    IsolateHeap::SetCurrent(heap_.get());
    realm_ = std::make_unique<Realm>(*heap_);
    event_loop_ = std::make_unique<EventLoop>();
    module_loader_ = std::make_unique<ModuleLoader>(*this);
    module_loader_->InitializeModuleGlobals();
}

Agent::~Agent() {
    if (s_current_agent_ == this) {
        s_current_agent_ = nullptr;
    }
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

    TaggedValue result = interpreter_.Execute(*bc);
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

TaggedValue Agent::EvaluateScript(std::string_view source) {
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

    TaggedValue result = interpreter_.Execute(*bc);
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

} // namespace aether
