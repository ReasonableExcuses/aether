#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include <sstream>

namespace aether {

bool TaggedValue::ToBoolean() const noexcept {
    if (IsSmi()) {
        return AsSmi() != 0;
    }
    if (IsDouble()) {
        double d = AsDouble();
        return !std::isnan(d) && d != 0.0;
    }
    if (IsBoolean()) {
        return AsBoolean();
    }
    if (IsNull() || IsUndefined() || IsEmpty()) {
        return false;
    }
    if (IsHeapObject()) {
        HeapObject* obj = AsHeapObject();
        if (obj->IsString()) {
            return static_cast<JSString*>(obj)->Length() > 0;
        }
        if (obj->IsBigInt()) {
            return !static_cast<JSBigInt*>(obj)->IsZero();
        }
        // In ECMAScript, all objects (except document.all legacy) are truthy
        return true;
    }
    return false;
}

double TaggedValue::ToDouble() const {
    if (IsSmi()) return static_cast<double>(AsSmi());
    if (IsDouble()) return AsDouble();
    if (IsBoolean()) return AsBoolean() ? 1.0 : 0.0;
    if (IsNull()) return 0.0;
    if (IsUndefined()) return std::numeric_limits<double>::quiet_NaN();
    if (IsHeapObject()) {
        HeapObject* obj = AsHeapObject();
        if (obj->IsString()) {
            std::string_view sv = static_cast<JSString*>(obj)->View();
            size_t start = sv.find_first_not_of(" \t\n\r");
            if (start == std::string_view::npos) return 0.0;
            size_t end = sv.find_last_not_of(" \t\n\r");
            std::string trimmed(sv.substr(start, end - start + 1));

            // Check for Infinity with exact case sensitivity
            if (trimmed.find('i') != std::string::npos || trimmed.find('I') != std::string::npos) {
                std::string_view sv_inf = trimmed;
                bool neg = false;
                if (!sv_inf.empty() && sv_inf.front() == '+') {
                    sv_inf.remove_prefix(1);
                } else if (!sv_inf.empty() && sv_inf.front() == '-') {
                    neg = true;
                    sv_inf.remove_prefix(1);
                }
                size_t s = sv_inf.find_first_not_of(" \t\n\r");
                if (s != std::string_view::npos) sv_inf.remove_prefix(s);
                if (sv_inf == "Infinity") {
                    return neg ? -std::numeric_limits<double>::infinity() : std::numeric_limits<double>::infinity();
                }
                return std::numeric_limits<double>::quiet_NaN();
            }

            // Check for binary, octal, hex integer literals (signs not allowed in StringNumericLiteral for 0b/0o/0x)
            if (trimmed.size() >= 3 && trimmed[0] == '0') {
                char type = trimmed[1];
                if (type == 'b' || type == 'B') {
                    try {
                        size_t idx = 0;
                        unsigned long long v = std::stoull(trimmed.substr(2), &idx, 2);
                        if (idx == trimmed.size() - 2) return static_cast<double>(v);
                    } catch (...) {}
                    return std::numeric_limits<double>::quiet_NaN();
                }
                if (type == 'o' || type == 'O') {
                    try {
                        size_t idx = 0;
                        unsigned long long v = std::stoull(trimmed.substr(2), &idx, 8);
                        if (idx == trimmed.size() - 2) return static_cast<double>(v);
                    } catch (...) {}
                    return std::numeric_limits<double>::quiet_NaN();
                }
                if (type == 'x' || type == 'X') {
                    try {
                        size_t idx = 0;
                        unsigned long long v = std::stoull(trimmed.substr(2), &idx, 16);
                        if (idx == trimmed.size() - 2) return static_cast<double>(v);
                    } catch (...) {}
                    return std::numeric_limits<double>::quiet_NaN();
                }
            }

            try {
                size_t idx = 0;
                double val = std::stod(trimmed, &idx);
                if (idx == trimmed.size()) return val;
                return std::numeric_limits<double>::quiet_NaN();
            } catch (...) {
                return std::numeric_limits<double>::quiet_NaN();
            }
        }
    }
    return std::numeric_limits<double>::quiet_NaN();
}

bool TaggedValue::IsSymbol() const noexcept {
    return (raw_ & kTagMask) == kTagSymbol || (IsHeapObject() && AsHeapObject()->GetType() == HeapObjectType::Symbol);
}

bool TaggedValue::IsString() const noexcept {
    return IsHeapObject() && AsHeapObject()->IsString();
}

bool TaggedValue::IsGeneratorObject() const noexcept {
    return IsHeapObject() && AsHeapObject()->GetType() == HeapObjectType::JSGenerator;
}

bool TaggedValue::IsBigInt() const noexcept {
    return IsHeapObject() && AsHeapObject()->GetType() == HeapObjectType::JSBigInt;
}

std::string TaggedValue::ToString() const {
    if (IsSmi()) {
        return std::to_string(AsSmi());
    }
    if (IsDouble()) {
        double d = AsDouble();
        if (std::isnan(d)) return "NaN";
        if (std::isinf(d)) return d > 0 ? "Infinity" : "-Infinity";
        std::ostringstream ss;
        ss << d;
        return ss.str();
    }
    if (IsBoolean()) {
        return AsBoolean() ? "true" : "false";
    }
    if (IsUndefined()) {
        return "undefined";
    }
    if (IsNull()) {
        return "null";
    }
    if (IsEmpty()) {
        return "<empty>";
    }
    if (IsHeapObject()) {
        HeapObject* obj = AsHeapObject();
        if (obj->IsString()) {
            return static_cast<JSString*>(obj)->Value();
        }
        if (obj->IsBigInt()) {
            return static_cast<JSBigInt*>(obj)->StringValue();
        }
        if (obj->GetType() == HeapObjectType::Symbol) {
            return "Symbol(" + static_cast<JSSymbol*>(obj)->Description() + ")";
        }
        if (obj->IsNativeFunction()) {
            return "[Function " + static_cast<JSNativeFunction*>(obj)->Name() + "]";
        }
        if (obj->GetType() == HeapObjectType::JSFunction) {
            return "[Function " + static_cast<JSFunction*>(obj)->Name() + "]";
        }
        if (obj->IsError()) {
            auto* err = static_cast<JSError*>(obj);
            return err->Name() + ": " + err->Message();
        }
        if (obj->IsProxy()) {
            auto* p = static_cast<JSProxy*>(obj);
            if (p->IsRevoked()) return "[object Object]";
            return p->GetTarget().ToString();
        }
        if (obj->IsJSObject()) {
            auto* jso = static_cast<JSObject*>(obj);
            if (jso->HasNamedProperty("__string_data__")) {
                return jso->GetNamedProperty("__string_data__").ToString();
            }
            if (jso->HasNamedProperty("__boolean_data__")) {
                return jso->GetNamedProperty("__boolean_data__").ToString();
            }
            if (jso->HasNamedProperty("__number_data__")) {
                return jso->GetNamedProperty("__number_data__").ToString();
            }
            if (jso->HasNamedProperty("__bigint_data__")) {
                return jso->GetNamedProperty("__bigint_data__").ToString();
            }
            return "[object Object]";
        }
        return "[HeapObject]";
    }
    return "<unknown>";
}

} // namespace aether
