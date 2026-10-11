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
        // In ECMAScript, all objects (except document.all legacy) are truthy
        return true;
    }
    return false;
}

bool TaggedValue::IsSymbol() const noexcept {
    return (raw_ & kTagMask) == kTagSymbol || (IsHeapObject() && AsHeapObject()->GetType() == HeapObjectType::Symbol);
}

bool TaggedValue::IsGeneratorObject() const noexcept {
    return IsHeapObject() && AsHeapObject()->GetType() == HeapObjectType::JSGenerator;
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
        if (obj->IsJSObject()) {
            return "[object Object]";
        }
        return "[HeapObject]";
    }
    return "<unknown>";
}

} // namespace aether
