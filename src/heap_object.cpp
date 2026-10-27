#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/typed_array.h"
#include "aether/bytecode.h"
#include "aether/bounded_regex.h"
#include <cctype>
#include <cmath>

namespace aether {

namespace {
thread_local size_t property_lookup_depth = 0;
struct PropertyLookupScope {
    PropertyLookupScope() { ++property_lookup_depth; }
    ~PropertyLookupScope() { --property_lookup_depth; }
};
} // namespace

static bool IsAllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

static bool TryParseArrayIndex(std::string_view s, size_t& index) noexcept {
    if (s.empty()) return false;
    size_t value = 0;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
        const size_t digit = static_cast<size_t>(c - '0');
        if (value > (std::numeric_limits<size_t>::max() - digit) / 10) {
            return false;
        }
        value = value * 10 + digit;
    }
    index = value;
    return true;
}

static bool SameValueZero(TaggedValue a, TaggedValue b) {
    if (a == b) return true;
    if (a.IsNumber() && b.IsNumber()) {
        double da = a.AsNumber();
        double db = b.AsNumber();
        if (std::isnan(da) && std::isnan(db)) return true;
        return da == db;
    }
    if (a.IsHeapObject() && b.IsHeapObject()) {
        HeapObject* ha = a.AsHeapObject();
        HeapObject* hb = b.AsHeapObject();
        if (ha->IsString() && hb->IsString()) {
            return static_cast<JSString*>(ha)->View() == static_cast<JSString*>(hb)->View();
        }
        return ha == hb;
    }
    return false;
}

TaggedValue JSObject::GetNamedProperty(std::string_view key) const {
    if (property_lookup_depth > kMaxPrototypeChainDepth) return TaggedValue::Undefined();
    PropertyLookupScope lookup_scope;
    if (deleted_properties_.count(std::string(key))) {
        if (prototype_ != nullptr) {
            return prototype_->GetNamedProperty(key);
        }
        return TaggedValue::Undefined();
    }
    if (IsArray()) {
        if (key == "length") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSArray*>(this)->Length()));
        }
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            if (idx < arr->Length() && !arr->Elements()[idx].IsEmpty()) {
                return arr->Elements()[idx];
            }
        }
    } else if (IsMap()) {
        if (key == "size") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSMap*>(this)->Size()));
        }
    } else if (IsSet()) {
        if (key == "size") {
            return TaggedValue::FromSmi(static_cast<int32_t>(static_cast<const JSSet*>(this)->Size()));
        }
    } else if (IsArrayBuffer()) {
        auto* ab = static_cast<const JSArrayBuffer*>(this);
        if (key == "byteLength") {
            return TaggedValue::FromDouble(static_cast<double>(ab->ByteLength()));
        }
        if (key == "maxByteLength") {
            return TaggedValue::FromDouble(static_cast<double>(ab->MaxByteLength()));
        }
        if (key == "growable") {
            return TaggedValue::FromBoolean(ab->IsGrowable());
        }
    } else if (IsTypedArray()) {
        auto* ta = static_cast<const JSTypedArray*>(this);
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            if (idx < ta->Length()) {
                return ta->GetElement(idx);
            }
            return TaggedValue::Undefined();
        }
    }

    if (const AccessorDescriptor* acc = GetAccessor(key)) {
        if (acc->getter.IsHeapObject() && acc->getter.AsHeapObject()->IsFunction()) {
            return CallFunction(acc->getter, TaggedValue::FromHeapObject(const_cast<JSObject*>(this)), {});
        }
        return TaggedValue::Undefined();
    }

    Shape* shape = GetShape();
    if (shape == nullptr && Realm::Current()) {
        shape = Realm::Current()->GetRootShape();
        const_cast<JSObject*>(this)->SetShape(shape);
    }
    if (AETHER_LIKELY(shape != nullptr)) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) {
                return val;
            }
        }

        if (IsFunction()) {
            if (key == "name") {
                if (GetType() == HeapObjectType::JSFunction) {
                    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<const JSFunction*>(this)->Name()));
                } else if (GetType() == HeapObjectType::JSNativeFunction) {
                    return TaggedValue::FromHeapObject(AllocateRuntime<JSString>(static_cast<const JSNativeFunction*>(this)->Name()));
                }
            }
            if (key == "length") {
                if (GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<const JSFunction*>(this);
                    uint32_t len = fn->GetBytecode() ? fn->GetBytecode()->Length() : 0;
                    return TaggedValue::FromSmi(static_cast<int32_t>(len));
                } else if (GetType() == HeapObjectType::JSNativeFunction) {
                    auto* fn = static_cast<const JSNativeFunction*>(this);
                    return TaggedValue::FromSmi(static_cast<int32_t>(fn->Length()));
                }
            }
        }

        if (IsUserFunction() && key == "prototype") {
            auto* fn = const_cast<JSFunction*>(static_cast<const JSFunction*>(this));
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                if (fn->GetConstructorPrototype().IsEmpty()) {
                    return TaggedValue::FromHeapObject(fn->GetOrCreateConstructorPrototype());
                }
                return fn->GetConstructorPrototype();
            }
            return TaggedValue::Undefined();
        }

        // Check instance prototype if explicitly assigned
        if (prototype_ != nullptr) {
            TaggedValue val = prototype_->GetNamedProperty(key);
            if (!val.IsUndefined()) {
                return val;
            }
        }

        // Walk shape prototype chain
        TaggedValue proto = shape->Prototype();
        size_t depth = 0;
        while (proto.IsHeapObject() && depth++ < kMaxPrototypeChainDepth) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
                if (const AccessorDescriptor* p_acc = js_proto->GetAccessor(key)) {
                    if (p_acc->getter.IsHeapObject() && p_acc->getter.AsHeapObject()->IsFunction()) {
                        return CallFunction(p_acc->getter, TaggedValue::FromHeapObject(const_cast<JSObject*>(this)), {});
                    }
                    return TaggedValue::Undefined();
                }
                Shape* p_shape = js_proto->GetShape();
                if (p_shape != nullptr) {
                    const PropertyDescriptor* p_desc = p_shape->LookupProperty(key);
                    if (p_desc != nullptr) {
                        TaggedValue p_val = js_proto->GetFastSlot(p_desc->slot_index, p_desc->is_in_object);
                        if (!p_val.IsEmpty()) {
                            return p_val;
                        }
                    }
                }
                proto = p_shape ? p_shape->Prototype() : TaggedValue::Null();
            } else {
                break;
            }
        }
    }
    return TaggedValue::Undefined();
}

bool JSObject::HasNamedProperty(std::string_view key) const {
    if (property_lookup_depth > kMaxPrototypeChainDepth) return false;
    PropertyLookupScope lookup_scope;
    if (deleted_properties_.count(std::string(key))) {
        if (prototype_ != nullptr && prototype_->HasNamedProperty(key)) return true;
        return false;
    }
    if (IsArray()) {
        if (key == "length") return true;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            if (idx < arr->Length() && !arr->Elements()[idx].IsEmpty()) {
                return true;
            }
        }
    } else if (IsMap()) {
        if (key == "size") return true;
    } else if (IsSet()) {
        if (key == "size") return true;
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            return idx < static_cast<const JSTypedArray*>(this)->Length();
        }
    }

    if (HasAccessor(key)) return true;

    Shape* shape = GetShape();
    if (shape == nullptr && Realm::Current()) {
        shape = Realm::Current()->GetRootShape();
    }
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) return true;
        }

        if (prototype_ != nullptr && prototype_->HasNamedProperty(key)) {
            return true;
        }

        if (IsUserFunction() && key == "prototype") {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                return true;
            }
            return false;
        }

        TaggedValue proto = shape->Prototype();
        size_t depth = 0;
        while (proto.IsHeapObject() && depth++ < kMaxPrototypeChainDepth) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
                if (js_proto->HasAccessor(key)) return true;
                Shape* p_shape = js_proto->GetShape();
                if (p_shape != nullptr) {
                    const PropertyDescriptor* p_desc = p_shape->LookupProperty(key);
                    if (p_desc != nullptr) {
                        TaggedValue p_val = js_proto->GetFastSlot(p_desc->slot_index, p_desc->is_in_object);
                        if (!p_val.IsEmpty()) return true;
                    }
                }
                proto = p_shape ? p_shape->Prototype() : TaggedValue::Null();
            } else {
                break;
            }
        }
    }
    return false;
}

bool JSObject::DeleteNamedProperty(std::string_view key) {
    if (IsArray()) {
        if (key == "length") return false;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            auto* arr = static_cast<JSArray*>(this);
            if (idx < arr->Length()) {
                PropertyAttributes attrs = GetOwnPropertyAttributes(key);
                if (attrs & PropertyAttributes::Configurable) {
                    arr->SetElement(idx, TaggedValue::Empty());
                    return true;
                }
                return false;
            }
        }
    }
    if (IsFunction()) {
        if (key == "length" || key == "name") {
            PropertyAttributes attrs = GetOwnPropertyAttributes(key);
            if (attrs & PropertyAttributes::Configurable) {
                MarkDeletedProperty(key);
                if (auto it = own_attributes_.find(key); it != own_attributes_.end()) own_attributes_.erase(it);
                return true;
            }
            return false;
        }
    }
    if (auto it = accessors_.find(std::string(key)); it != accessors_.end()) {
        PropertyAttributes attrs = it->second.attributes;
        auto own_it = own_attributes_.find(std::string(key));
        if (own_it != own_attributes_.end()) {
            attrs = own_it->second;
        }
        if (attrs & PropertyAttributes::Configurable) {
            MarkDeletedProperty(key);
            accessors_.erase(it);
            if (own_it != own_attributes_.end()) own_attributes_.erase(own_it);
            return true;
        }
        return false;
    }
    Shape* shape = GetShape();
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) {
                PropertyAttributes attrs = GetOwnPropertyAttributes(key);
                if (attrs & PropertyAttributes::Configurable) {
                    MarkDeletedProperty(key);
                    if (auto it = own_attributes_.find(key); it != own_attributes_.end()) own_attributes_.erase(it);
                    SetFastSlot(desc->slot_index, desc->is_in_object, TaggedValue::Empty());
                    return true;
                }
                return false;
            }
        }
    }
    return true;
}

bool JSObject::HasOwnProperty(std::string_view key) const {
    if (deleted_properties_.count(std::string(key))) return false;
    if (accessors_.find(std::string(key)) != accessors_.end()) return true;
    Shape* shape = GetShape();
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
            if (!val.IsEmpty()) return true;
        }
    }
    if (IsArray()) {
        if (key == "length") return true;
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            const auto* arr = static_cast<const JSArray*>(this);
            return idx < arr->Length() && !arr->Elements()[idx].IsEmpty();
        }
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
        if (GetType() == HeapObjectType::JSFunction) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (key == "prototype" && !fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) return true;
        }
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx)) {
            return idx < static_cast<const JSTypedArray*>(this)->Length();
        }
    }
    return false;
}

PropertyAttributes JSObject::GetOwnPropertyAttributes(std::string_view key) const {
    auto override_it = own_attributes_.find(std::string(key));
    PropertyAttributes attrs = PropertyAttributes::None;
    if (override_it != own_attributes_.end()) {
        attrs = override_it->second;
    } else if (const AccessorDescriptor* accessor = GetAccessor(key)) {
        attrs = accessor->attributes;
    } else if (IsArray()) {
        size_t idx = 0;
        if (key == "length") {
            attrs = PropertyAttributes::Writable;
        } else if (TryParseArrayIndex(key, idx)) {
            if (idx < static_cast<const JSArray*>(this)->Length()) {
                attrs = PropertyAttributes::Default;
            }
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else if (IsFunction()) {
        if (key == "name" || key == "length") {
            attrs = PropertyAttributes::Configurable;
        } else if (key == "prototype" && IsUserFunction()) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync() && !fn->IsMethod()) {
                attrs = PropertyAttributes::Writable;
            }
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else if (IsTypedArray()) {
        size_t idx = 0;
        if (TryParseArrayIndex(key, idx) && idx < static_cast<const JSTypedArray*>(this)->Length()) {
            attrs = PropertyAttributes::Writable | PropertyAttributes::Enumerable;
        } else {
            Shape* shape = GetShape();
            if (shape != nullptr) {
                if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                    TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                    if (!val.IsEmpty()) {
                        attrs = desc->attributes;
                    }
                }
            }
        }
    } else {
        Shape* shape = GetShape();
        if (shape != nullptr) {
            if (const PropertyDescriptor* desc = shape->LookupProperty(key)) {
                TaggedValue val = GetFastSlot(desc->slot_index, desc->is_in_object);
                if (!val.IsEmpty()) {
                    attrs = desc->attributes;
                }
            }
        }
    }

    uint8_t bits = static_cast<uint8_t>(attrs);
    if (is_frozen_) {
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Writable);
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Configurable);
    } else if (is_sealed_) {
        bits &= ~static_cast<uint8_t>(PropertyAttributes::Configurable);
    }
    return static_cast<PropertyAttributes>(bits);
}

void JSObject::SetOwnPropertyAttributes(std::string_view key, PropertyAttributes attributes) {
    CheckPropertyGrowth(key);
    if (!own_attributes_.contains(key) && own_attributes_.size() >= kMaxObjectProperties) {
        throw std::length_error("object property limit exceeded");
    }
    own_attributes_.insert_or_assign(detail::MakePropertyKey(key, key_allocator_), attributes);
    auto accessor = accessors_.find(key);
    if (accessor != accessors_.end()) accessor->second.attributes = attributes;
}

void JSObject::CheckPropertyGrowth(std::string_view key) const {
    if (key.size() > kMaxStringLength) throw std::length_error("property key is too long");
    const Shape* shape = GetShape();
    if (accessors_.contains(key) || own_attributes_.contains(key) ||
        (shape && shape->LookupProperty(key))) return;
    // own_attributes_ includes both data and accessor names. Shapes retain
    // deleted slots, so separately enforce the slot count at transition time.
    if (own_attributes_.size() >= kMaxObjectProperties ||
        accessors_.size() >= kMaxObjectProperties) {
        throw std::length_error("object property limit exceeded");
    }
}

void JSObject::Freeze() noexcept {
    is_frozen_ = true;
    is_sealed_ = true;
    is_extensible_ = false;
}

void JSObject::Seal() noexcept {
    is_sealed_ = true;
    is_extensible_ = false;
}

std::vector<std::string> JSObject::GetOwnPropertyNames() const {
    std::vector<std::string> indices;
    std::vector<std::string> string_keys;
    std::vector<std::string> symbol_keys;
    std::unordered_set<std::string> seen;

    auto add_key = [&](const std::string& name) {
        if (name.rfind("__private__", 0) == 0) return;
        if (deleted_properties_.count(name)) return;
        if (seen.insert(name).second) {
            size_t parsed_index = 0;
            if (TryParseArrayIndex(name, parsed_index)) {
                indices.push_back(name);
            } else if (name.rfind("@@", 0) == 0) {
                symbol_keys.push_back(name);
            } else {
                string_keys.push_back(name);
            }
        }
    };

    if (IsArray()) {
        const auto* arr = static_cast<const JSArray*>(this);
        for (size_t i = 0; i < arr->Length(); ++i) {
            if (!arr->Elements()[i].IsEmpty()) {
                add_key(std::to_string(i));
            }
        }
        add_key("length");
    } else if (IsFunction()) {
        add_key("length");
        add_key("name");
        if (IsUserFunction()) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (!fn->IsArrow() && !fn->IsAsync()) {
                add_key("prototype");
            }
        }
    }

    // Collect accessor names
    for (const auto& [name, acc] : accessors_) {
        add_key(std::string(name));
    }

    // Collect shape property names ordered by slot_index (creation order)
    Shape* shape = GetShape();
    if (shape != nullptr) {
        std::vector<std::pair<uint32_t, std::string>> ordered_props;
        for (const auto& [name, desc] : shape->GetPropertyMap()) {
            TaggedValue val = GetFastSlot(desc.slot_index, desc.is_in_object);
            if (!val.IsEmpty()) {
                ordered_props.emplace_back(desc.slot_index, name);
            }
        }
        std::sort(ordered_props.begin(), ordered_props.end(), [](const auto& a, const auto& b) {
            return a.first < b.first;
        });
        for (const auto& [slot, name] : ordered_props) {
            add_key(name);
        }
    }

    // Sort integer indices ascending numerically
    std::sort(indices.begin(), indices.end(), [](const std::string& a, const std::string& b) {
        size_t ai = 0;
        size_t bi = 0;
        const bool a_ok = TryParseArrayIndex(a, ai);
        const bool b_ok = TryParseArrayIndex(b, bi);
        if (a_ok && b_ok) return ai < bi;
        return a < b;
    });

    std::vector<std::string> result;
    result.reserve(indices.size() + string_keys.size() + symbol_keys.size());
    result.insert(result.end(), indices.begin(), indices.end());
    result.insert(result.end(), string_keys.begin(), string_keys.end());
    result.insert(result.end(), symbol_keys.begin(), symbol_keys.end());
    return result;
}
void JSObject::SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id) {
    CheckPropertyGrowth(key);
    Shape* current_shape = GetShape();
    if (current_shape == nullptr) {
        if (Realm::Current()) {
            current_shape = Realm::Current()->GetRootShape();
        } else {
            static Shape s_fallback_root_shape(0, nullptr, "", TaggedValue::Null());
            current_shape = &s_fallback_root_shape;
        }
    }
    assert(current_shape != nullptr && "Object must have a valid shape");

    if (is_frozen_) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
        }
        return;
    }

    bool is_private = key.rfind("__private__", 0) == 0;
    if (const AccessorDescriptor* acc = GetAccessor(key)) {
        if (acc->setter.IsHeapObject() && acc->setter.AsHeapObject()->IsFunction()) {
            CallFunction(acc->setter, TaggedValue::FromHeapObject(this), {value});
        } else if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
            if (Interpreter::Current()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot set property without a setter")));
            }
        }
        return;
    }

    if (IsFunction()) {
        if (key == "length" || key == "name") {
            if (Interpreter::Current() != nullptr) {
                auto override_it = own_attributes_.find(std::string(key));
                if (override_it == own_attributes_.end() || !(override_it->second & PropertyAttributes::Writable)) {
                    if (Interpreter::Current()->IsStrictMode()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot assign to read-only property '" + std::string(key) + "'")));
                    }
                    return;
                }
            } else if (key == "length" && GetType() == HeapObjectType::JSNativeFunction && value.IsSmi()) {
                static_cast<JSNativeFunction*>(this)->SetLength(static_cast<uint32_t>(value.AsSmi()));
            }
        }
    }

    if (IsUserFunction() && key == "prototype") {
        static_cast<JSFunction*>(this)->SetConstructorPrototype(value);
        return;
    }

    if (IsArray()) {
        if (key == "length") {
            const double length = value.ToDouble();
            if (!std::isfinite(length) || length < 0 ||
                length > static_cast<double>(kMaxArrayLength) || std::trunc(length) != length) {
                throw std::length_error("invalid array length");
            }
            static_cast<JSArray*>(this)->SetLength(static_cast<size_t>(length));
            ClearDeletedProperty(key);
            return;
        }
        if (IsAllDigits(key)) {
            size_t idx = 0;
            if (!TryParseArrayIndex(key, idx)) {
                throw std::length_error("array index overflow");
            }
            static_cast<JSArray*>(this)->SetElement(idx, value);
            ClearDeletedProperty(key);
            return;
        }
    }

    const PropertyDescriptor* desc = current_shape->LookupProperty(key);
    if (desc != nullptr) {
        TaggedValue cur_val = GetFastSlot(desc->slot_index, desc->is_in_object);
        if (!cur_val.IsEmpty()) {
            if (!(GetOwnPropertyAttributes(key) & PropertyAttributes::Writable)) {
                if (Interpreter::Current() && (is_private || Interpreter::Current()->IsStrictMode())) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
                }
                return;
            }
            // Fast path: Shape already contains property; overwrite existing slot
            SetFastSlot(desc->slot_index, desc->is_in_object, value);
            ClearDeletedProperty(key);
            return;
        }
    }

    // A setter or non-writable property on the prototype chain handles assignment
    TaggedValue proto_val = prototype_ ? TaggedValue::FromHeapObject(prototype_) : (current_shape ? current_shape->Prototype() : TaggedValue::Null());
    size_t prototype_depth = 0;
    while (proto_val.IsHeapObject()) {
        if (++prototype_depth > kMaxPrototypeChainDepth) {
            throw std::length_error("prototype chain depth exceeded");
        }
        HeapObject* p_obj = proto_val.AsHeapObject();
        if (p_obj->IsJSObject()) {
            JSObject* p = static_cast<JSObject*>(p_obj);
            if (const AccessorDescriptor* acc = p->GetAccessor(key)) {
                if (acc->setter.IsHeapObject() && acc->setter.AsHeapObject()->IsFunction()) {
                    CallFunction(acc->setter, TaggedValue::FromHeapObject(this), {value});
                } else if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            AllocateRuntime<JSError>("TypeError", "Cannot set property without a setter")));
                    }
                }
                return;
            }
            if (is_private && p->HasOwnProperty(key)) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        AllocateRuntime<JSError>("TypeError", "Cannot assign to private method")));
                }
                return;
            }
            if (p->HasOwnProperty(key)) {
                PropertyAttributes attrs = p->GetOwnPropertyAttributes(key);
                if (!(attrs & PropertyAttributes::Writable)) {
                    if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                        if (Interpreter::Current()) {
                            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                                AllocateRuntime<JSError>("TypeError", "Cannot assign to read only property")));
                        }
                    }
                    return;
                }
                break;
            }
            proto_val = p->GetPrototype() ? TaggedValue::FromHeapObject(p->GetPrototype()) : (p->GetShape() ? p->GetShape()->Prototype() : TaggedValue::Null());
        } else {
            break;
        }
    }

    if (desc != nullptr) {
        // Slot is Empty() (property was deleted). Check extensibility before re-adding!
        if (is_sealed_ || !is_extensible_) {
            if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
                Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                    AllocateRuntime<JSError>("TypeError", "Cannot add property to a non-extensible object")));
            }
            return;
        }
        PrepareFastSlot(desc->slot_index, desc->is_in_object);
        if (!own_attributes_.contains(key)) SetOwnPropertyAttributes(key, PropertyAttributes::Default);
        SetFastSlot(desc->slot_index, desc->is_in_object, value);
        ClearDeletedProperty(key);
        return;
    }
    if (is_sealed_ || !is_extensible_) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                AllocateRuntime<JSError>("TypeError", "Cannot add property to a non-extensible object")));
        }
        return;
    }

    // The transition allocates the next slot at PropertyCount().  Validate
    // and reserve it before publishing the child shape so a failed growth
    // cannot leave a shape whose slot metadata points past storage.
    if (AETHER_UNLIKELY(current_shape->PropertyCount() >= kMaxObjectProperties)) {
        throw std::length_error("object property limit exceeded");
    }
    const uint32_t next_slot = current_shape->PropertyCount();
    PrepareFastSlot(next_slot, next_slot < Shape::kDefaultInObjectProperties);
    auto [attr, inserted] = own_attributes_.try_emplace(detail::MakePropertyKey(key, key_allocator_), PropertyAttributes::Default);
    Shape* next_shape;
    try {
        next_shape = current_shape->FindOrCreateTransition(key, next_shape_id, current_shape->Prototype());
    } catch (...) {
        if (inserted) own_attributes_.erase(attr);
        throw;
    }
    // All allocating operations have succeeded; publish storage before shape.
    SetFastSlot(next_slot, next_slot < Shape::kDefaultInObjectProperties, value);
    SetShape(next_shape);
    ++next_shape_id;
    ClearDeletedProperty(key);
}

// JSMap Implementation
void JSMap::Set(TaggedValue key, TaggedValue val) {
    for (auto& entry : entries_) {
        if (SameValueZero(entry.first, key)) {
            entry.second = val;
            if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
                RecordWriteBarrier(this, val);
            }
            return;
        }
    }
    detail::ReserveStorage(entries_, entries_storage_, entries_.size() + 1);
    entries_.emplace_back(key, val);
    if (AETHER_UNLIKELY(IsOld())) {
        if (key.IsHeapObject()) RecordWriteBarrier(this, key);
        if (val.IsHeapObject()) RecordWriteBarrier(this, val);
    }
}

TaggedValue JSMap::Get(TaggedValue key) const {
    for (const auto& entry : entries_) {
        if (SameValueZero(entry.first, key)) {
            return entry.second;
        }
    }
    return TaggedValue::Undefined();
}

bool JSMap::Has(TaggedValue key) const {
    for (const auto& entry : entries_) {
        if (SameValueZero(entry.first, key)) {
            return true;
        }
    }
    return false;
}

bool JSMap::Delete(TaggedValue key) {
    for (auto it = entries_.begin(); it != entries_.end(); ++it) {
        if (SameValueZero(it->first, key)) {
            entries_.erase(it);
            return true;
        }
    }
    return false;
}

// JSSet Implementation
void JSSet::Add(TaggedValue val) {
    for (const auto& v : values_) {
        if (SameValueZero(v, val)) {
            return;
        }
    }
    detail::ReserveStorage(values_, values_storage_, values_.size() + 1);
    values_.push_back(val);
    if (AETHER_UNLIKELY(IsOld() && val.IsHeapObject())) {
        RecordWriteBarrier(this, val);
    }
}

bool JSSet::Has(TaggedValue val) const {
    for (const auto& v : values_) {
        if (SameValueZero(v, val)) {
            return true;
        }
    }
    return false;
}

bool JSSet::Delete(TaggedValue val) {
    for (auto it = values_.begin(); it != values_.end(); ++it) {
        if (SameValueZero(*it, val)) {
            values_.erase(it);
            return true;
        }
    }
    return false;
}

// JSPromise Implementation
void JSPromise::Fulfill(TaggedValue val, MicrotaskQueue& queue) {
    if (state_ != PromiseState::Pending) return;
    state_ = PromiseState::Fulfilled;
    result_ = val;

    for (const auto& reaction : reactions_) {
        auto on_fulfilled = reaction.on_fulfilled;
        auto* downstream = reaction.downstream_promise;
        queue.Enqueue([on_fulfilled, val, downstream, &queue]() {
            if (!on_fulfilled.IsEmpty() && !on_fulfilled.IsUndefined()) {
                TaggedValue res = CallFunction(on_fulfilled, TaggedValue::Undefined(), {val});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Fulfill(val, queue);
            }
        }, {on_fulfilled, val, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    }
    reactions_.clear();
}

void JSPromise::Reject(TaggedValue reason, MicrotaskQueue& queue) {
    if (state_ != PromiseState::Pending) return;
    state_ = PromiseState::Rejected;
    result_ = reason;

    for (const auto& reaction : reactions_) {
        auto on_rejected = reaction.on_rejected;
        auto* downstream = reaction.downstream_promise;
        queue.Enqueue([on_rejected, reason, downstream, &queue]() {
            if (!on_rejected.IsEmpty() && !on_rejected.IsUndefined()) {
                TaggedValue res = CallFunction(on_rejected, TaggedValue::Undefined(), {reason});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Reject(reason, queue);
            }
        }, {on_rejected, reason, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    }
    reactions_.clear();
}

void JSPromise::AddReaction(TaggedValue on_fulfilled, TaggedValue on_rejected, JSPromise* downstream, MicrotaskQueue& queue) {
    if (state_ == PromiseState::Fulfilled) {
        TaggedValue val = result_;
        queue.Enqueue([on_fulfilled, val, downstream, &queue]() {
            if (!on_fulfilled.IsEmpty() && !on_fulfilled.IsUndefined()) {
                TaggedValue res = CallFunction(on_fulfilled, TaggedValue::Undefined(), {val});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Fulfill(val, queue);
            }
        }, {on_fulfilled, val, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    } else if (state_ == PromiseState::Rejected) {
        TaggedValue reason = result_;
        queue.Enqueue([on_rejected, reason, downstream, &queue]() {
            if (!on_rejected.IsEmpty() && !on_rejected.IsUndefined()) {
                TaggedValue res = CallFunction(on_rejected, TaggedValue::Undefined(), {reason});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Reject(reason, queue);
            }
        }, {on_rejected, reason, downstream ? TaggedValue::FromHeapObject(downstream) : TaggedValue::Undefined()});
    } else {
        detail::ReserveStorage(reactions_, reactions_storage_, reactions_.size() + 1);
        reactions_.push_back({on_fulfilled, on_rejected, downstream});
    }
}

// JSRegExp Implementation
bool JSRegExp::Test(std::string_view str) const {
    return RegexSearch(Pattern(), Flags(), str).has_value();
}

void JSFunction::IteratePointers(const std::function<void(TaggedValue&)>& visitor) {
    JSObject::IteratePointers(visitor);
    if (!ctor_prototype_.IsEmpty()) {
        visitor(ctor_prototype_);
    }
    if (lexical_this_.IsHeapObject()) {
        visitor(lexical_this_);
    }
    if (lexical_new_target_.IsHeapObject()) {
        visitor(lexical_new_target_);
    }
    if (environment_ != nullptr) {
        TaggedValue env_val = TaggedValue::FromHeapObject(environment_);
        visitor(env_val);
        if (env_val.IsHeapObject()) {
            environment_ = env_val.As<LexicalEnvironment>();
        }
    }
    if (bytecode_) {
        const_cast<BytecodeArray*>(bytecode_)->IterateConstants(visitor);
    }
    for (auto*& obj : with_stack_) {
        if (obj) {
            TaggedValue obj_val = TaggedValue::FromHeapObject(obj);
            visitor(obj_val);
            if (obj_val.IsHeapObject() && obj_val.AsHeapObject()->IsJSObject()) {
                obj = static_cast<JSObject*>(obj_val.AsHeapObject());
            }
        }
    }
    for (auto& f : instance_fields_) {
        if (f.initial_value.IsHeapObject()) {
            visitor(f.initial_value);
        }
    }
}

JSFunction::JSFunction(std::string_view name, std::shared_ptr<const BytecodeArray> bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(detail::ChargedString(name, name_storage_)),
      bytecode_shared_(std::move(bytecode)),
      bytecode_(bytecode_shared_.get()),
      environment_(env) {
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSFunction::JSFunction(std::string_view name, const BytecodeArray* bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(detail::ChargedString(name, name_storage_)),
      bytecode_(bytecode),
      environment_(env) {
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSObject* JSFunction::GetOrCreateConstructorPrototype() {
    if (ctor_prototype_.IsEmpty()) {
        Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* proto = AllocateRuntime<JSObject>(root_shape);
        if (Realm::Current() && Realm::Current()->GetObjectPrototype()) {
            proto->SetPrototype(Realm::Current()->GetObjectPrototype());
        }
        uint32_t sid = Realm::Current() ? Realm::Current()->NextShapeId() : 1;
        proto->SetNamedProperty("constructor", TaggedValue::FromHeapObject(this), sid);
        ctor_prototype_ = TaggedValue::FromHeapObject(proto);
        return proto;
    }
    if (ctor_prototype_.IsHeapObject() && ctor_prototype_.AsHeapObject()->IsJSObject()) {
        return ctor_prototype_.As<JSObject>();
    }
    return nullptr;
}

JSError::JSError(std::string_view name, std::string_view message, Shape* shape)
    : JSObject(HeapObjectType::JSError, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(detail::ChargedString(name, name_storage_)),
      message_(detail::ChargedString(message, message_storage_)) {
    uint32_t sid = 8000;
    SetNamedProperty("name", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(name_)), sid);
    SetNamedProperty("message", TaggedValue::FromHeapObject(AllocateRuntime<JSString>(message_)), sid);
    if (Realm::Current()) {
        TaggedValue ctor_val = Realm::Current()->GetGlobalProperty(name_);
        if (!ctor_val.IsEmpty() && ctor_val.IsHeapObject()) {
            SetNamedProperty("constructor", ctor_val, sid);
            if (ctor_val.AsHeapObject()->IsJSObject()) {
                TaggedValue proto_val = static_cast<JSObject*>(ctor_val.AsHeapObject())->GetNamedProperty("prototype");
                if (proto_val.IsHeapObject() && proto_val.AsHeapObject()->IsJSObject()) {
                    SetPrototype(static_cast<JSObject*>(proto_val.AsHeapObject()));
                }
            }
        }
    }
}

JSBigInt* JSBigInt::Create(int64_t value) {
    return Create(JSBigInt(value));
}

JSBigInt* JSBigInt::Create(uint64_t value) {
    return Create(JSBigInt(value));
}

JSBigInt* JSBigInt::Create(bool negative, const std::vector<uint32_t>& limbs) {
    return Create(JSBigInt(negative, limbs));
}

JSBigInt* JSBigInt::Create(bool negative, std::vector<uint32_t>&& limbs) {
    return Create(JSBigInt(negative, std::move(limbs)));
}

JSBigInt* JSBigInt::Create(JSBigInt&& value) {
    return AllocateRuntime<JSBigInt>(std::move(value));
}

JSBigInt* JSBigInt::Create(bool negative, std::vector<uint32_t>&& limbs, MemoryCharge&& charge) {
    JSBigInt value(0);
    value.is_negative_ = negative;
    value.limbs_storage_ = std::move(charge);
    value.limbs_ = std::move(limbs);
    value.Normalize();
    return Create(std::move(value));
}

JSArrayBuffer* JSArrayBuffer::Slice(int64_t begin, int64_t end, Shape* shape) const {
    if (is_detached_) throw std::length_error("ArrayBuffer is detached");
    const int64_t len = static_cast<int64_t>(byte_length_);
    const size_t start = static_cast<size_t>(NormalizeIndex(begin, len));
    const size_t finish = static_cast<size_t>(NormalizeIndex(end, len));
    const size_t count = finish > start ? finish - start : 0;
    const bool shared = is_shared_;
    std::optional<HandleScope> scope;
    Handle<JSArrayBuffer> source;
    if (auto* heap = IsolateHeap::Current()) {
        scope.emplace(*heap);
        source = scope->CreateHandle(const_cast<JSArrayBuffer*>(this));
    }
    auto* copy = shared ? AllocateRuntime<JSArrayBuffer>(count, count, true, shape)
                        : AllocateRuntime<JSArrayBuffer>(count, shape);
    const auto* original = scope ? source.Get() : this;
    if (count != 0) std::memcpy(copy->Data(), original->Data() + start, count);
    return copy;
}

JSTypedArray* JSTypedArray::Subarray(int64_t begin, int64_t end, Shape* shape) const {
    if (!buffer_ || buffer_->IsDetached()) throw std::length_error("TypedArray buffer is detached");
    const int64_t len = static_cast<int64_t>(length_);
    const size_t start = static_cast<size_t>(NormalizeIndex(begin, len));
    const size_t finish = static_cast<size_t>(NormalizeIndex(end, len));
    const size_t count = finish > start ? finish - start : 0;
    const size_t delta = CheckedStorageBytes(start, BytesPerElement());
    if (delta > std::numeric_limits<size_t>::max() - byte_offset_) {
        throw std::length_error("TypedArray subarray offset overflow");
    }
    JSTypedArray pending(kind_, buffer_, byte_offset_ + delta, count, shape);
    JSArrayBuffer* buffer = buffer_;
    std::optional<HandleScope> scope;
    Handle<JSArrayBuffer> source;
    if (auto* heap = IsolateHeap::Current()) {
        scope.emplace(*heap);
        source = scope->CreateHandle(buffer);
    }
    // A managed allocation may move the rooted backing object. Publish the
    // refreshed pointer after allocation rather than passing a stale raw arg.
    pending.buffer_ = nullptr;
    auto* result = AllocateRuntime<JSTypedArray>(std::move(pending));
    result->buffer_ = scope ? source.Get() : buffer;
    if (result->IsOld()) RecordWriteBarrier(result, TaggedValue::FromHeapObject(result->buffer_));
    return result;
}

} // namespace aether
