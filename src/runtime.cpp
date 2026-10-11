#include "aether/runtime.h"
#include "aether/event_loop.h"
#include "aether/module.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
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
        if (Interpreter::Current()) {
            return Interpreter::Current()->Execute(*fn->GetBytecode(), args, this_val, fn->GetEnvironment());
        }
    }
    return TaggedValue::Undefined();
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

    // JSON Methods
    json_object_->SetNamedProperty("parse", TaggedValue::FromHeapObject(make_native("parse", &JSON_parse)), next_shape_id_);
    json_object_->SetNamedProperty("stringify", TaggedValue::FromHeapObject(make_native("stringify", &JSON_stringify)), next_shape_id_);

    // Map Prototype & Constructor
    map_prototype_->SetNamedProperty("set", TaggedValue::FromHeapObject(make_native("set", &Map_set)), next_shape_id_);
    map_prototype_->SetNamedProperty("get", TaggedValue::FromHeapObject(make_native("get", &Map_get)), next_shape_id_);
    map_prototype_->SetNamedProperty("has", TaggedValue::FromHeapObject(make_native("has", &Map_has)), next_shape_id_);
    map_prototype_->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_native("delete", &Map_delete)), next_shape_id_);
    map_prototype_->SetNamedProperty("clear", TaggedValue::FromHeapObject(make_native("clear", &Map_clear)), next_shape_id_);
    auto* map_ctor = make_native("Map", &Map_ctor);
    map_ctor->SetNamedProperty("prototype", TaggedValue::FromHeapObject(map_prototype_), next_shape_id_);

    // Set Prototype & Constructor
    set_prototype_->SetNamedProperty("add", TaggedValue::FromHeapObject(make_native("add", &Set_add)), next_shape_id_);
    set_prototype_->SetNamedProperty("has", TaggedValue::FromHeapObject(make_native("has", &Set_has)), next_shape_id_);
    set_prototype_->SetNamedProperty("delete", TaggedValue::FromHeapObject(make_native("delete", &Set_delete)), next_shape_id_);
    set_prototype_->SetNamedProperty("clear", TaggedValue::FromHeapObject(make_native("clear", &Set_clear)), next_shape_id_);
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
