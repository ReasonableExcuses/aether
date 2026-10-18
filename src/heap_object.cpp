#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/typed_array.h"
#include "aether/bytecode.h"
#include <cctype>
#include <regex>
#include <cmath>

namespace aether {

static bool IsAllDigits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
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
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            return static_cast<const JSArray*>(this)->GetElement(idx);
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
                    return TaggedValue::FromHeapObject(new JSString(static_cast<const JSFunction*>(this)->Name()));
                } else if (GetType() == HeapObjectType::JSNativeFunction) {
                    return TaggedValue::FromHeapObject(new JSString(static_cast<const JSNativeFunction*>(this)->Name()));
                }
            }
            if (key == "length") {
                if (GetType() == HeapObjectType::JSFunction) {
                    auto* fn = static_cast<const JSFunction*>(this);
                    uint32_t len = fn->GetBytecode() ? fn->GetBytecode()->Length() : 0;
                    return TaggedValue::FromSmi(static_cast<int32_t>(len));
                }
            }
        }

        if (IsUserFunction() && key == "prototype") {
            auto* fn = const_cast<JSFunction*>(static_cast<const JSFunction*>(this));
            if (!fn->IsArrow() && !fn->IsAsync()) {
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
        while (proto.IsHeapObject()) {
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
    if (deleted_properties_.count(std::string(key))) {
        if (prototype_ != nullptr && prototype_->HasNamedProperty(key)) return true;
        return false;
    }
    if (IsArray()) {
        if (key == "length") return true;
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            const auto* arr = static_cast<const JSArray*>(this);
            return idx < arr->Length() && !arr->Elements()[idx].IsEmpty();
        }
    } else if (IsMap()) {
        if (key == "size") return true;
    } else if (IsSet()) {
        if (key == "size") return true;
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
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
            if (!fn->IsArrow() && !fn->IsAsync()) {
                return true;
            }
            return false;
        }

        TaggedValue proto = shape->Prototype();
        while (proto.IsHeapObject()) {
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
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            auto* arr = static_cast<JSArray*>(this);
            if (idx < arr->Length()) {
                arr->SetElement(idx, TaggedValue::Empty());
                return true;
            }
        }
    }
    if (IsFunction()) {
        if (key == "length" || key == "name") {
            PropertyAttributes attrs = GetOwnPropertyAttributes(key);
            if (attrs & PropertyAttributes::Configurable) {
                deleted_properties_.insert(std::string(key));
                own_attributes_.erase(std::string(key));
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
            accessors_.erase(it);
            own_attributes_.erase(std::string(key));
            deleted_properties_.insert(std::string(key));
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
                    own_attributes_.erase(std::string(key));
                    SetFastSlot(desc->slot_index, desc->is_in_object, TaggedValue::Empty());
                    deleted_properties_.insert(std::string(key));
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
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            const auto* arr = static_cast<const JSArray*>(this);
            return idx < arr->Length() && !arr->Elements()[idx].IsEmpty();
        }
    } else if (IsFunction()) {
        if (key == "length" || key == "name") return true;
        if (GetType() == HeapObjectType::JSFunction) {
            const auto* fn = static_cast<const JSFunction*>(this);
            if (key == "prototype" && !fn->IsArrow() && !fn->IsAsync()) return true;
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
        if (key == "length") {
            attrs = PropertyAttributes::Writable;
        } else if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
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
            if (!fn->IsArrow() && !fn->IsAsync()) {
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
    own_attributes_[std::string(key)] = attributes;
    auto accessor = accessors_.find(std::string(key));
    if (accessor != accessors_.end()) accessor->second.attributes = attributes;
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
            if (IsAllDigits(name)) {
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
        add_key(name);
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
        return std::stoull(a) < std::stoull(b);
    });

    std::vector<std::string> result;
    result.reserve(indices.size() + string_keys.size() + symbol_keys.size());
    result.insert(result.end(), indices.begin(), indices.end());
    result.insert(result.end(), string_keys.begin(), string_keys.end());
    result.insert(result.end(), symbol_keys.begin(), symbol_keys.end());
    return result;
}
void JSObject::SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id) {
    deleted_properties_.erase(std::string(key));
    Shape* current_shape = GetShape();
    if (current_shape == nullptr) {
        if (Realm::Current()) {
            current_shape = Realm::Current()->GetRootShape();
        } else {
            static Shape s_fallback_root_shape(0, nullptr, "", TaggedValue::Null());
            current_shape = &s_fallback_root_shape;
        }
        SetShape(current_shape);
    }
    assert(current_shape != nullptr && "Object must have a valid shape");

    if (is_frozen_) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                new JSError("TypeError", "Cannot assign to read only property")));
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
                    new JSError("TypeError", "Cannot set property without a setter")));
            }
        }
        return;
    }

    if (IsFunction()) {
        if (key == "length" || key == "name") {
            auto override_it = own_attributes_.find(std::string(key));
            if (override_it == own_attributes_.end() || !(override_it->second & PropertyAttributes::Writable)) {
                if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        new JSError("TypeError", "Cannot assign to read-only property '" + std::string(key) + "'")));
                }
                return;
            }
        }
    }

    if (IsUserFunction() && key == "prototype") {
        static_cast<JSFunction*>(this)->SetConstructorPrototype(value);
        return;
    }

    if (IsArray()) {
        if (key == "length") {
            if (value.IsSmi()) {
                static_cast<JSArray*>(this)->SetLength(static_cast<size_t>(value.AsSmi()));
            }
            return;
        }
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            static_cast<JSArray*>(this)->SetElement(idx, value);
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
                        new JSError("TypeError", "Cannot assign to read only property")));
                }
                return;
            }
            // Fast path: Shape already contains property; overwrite existing slot
            SetFastSlot(desc->slot_index, desc->is_in_object, value);
            return;
        }
    }

    // A setter or non-writable property on the prototype chain handles assignment
    TaggedValue proto_val = prototype_ ? TaggedValue::FromHeapObject(prototype_) : (current_shape ? current_shape->Prototype() : TaggedValue::Null());
    while (proto_val.IsHeapObject()) {
        HeapObject* p_obj = proto_val.AsHeapObject();
        if (p_obj->IsJSObject()) {
            JSObject* p = static_cast<JSObject*>(p_obj);
            if (const AccessorDescriptor* acc = p->GetAccessor(key)) {
                if (acc->setter.IsHeapObject() && acc->setter.AsHeapObject()->IsFunction()) {
                    CallFunction(acc->setter, TaggedValue::FromHeapObject(this), {value});
                } else if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            new JSError("TypeError", "Cannot set property without a setter")));
                    }
                }
                return;
            }
            if (is_private && p->HasNamedProperty(key)) {
                if (Interpreter::Current()) {
                    Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                        new JSError("TypeError", "Cannot assign to private method")));
                }
                return;
            }
            PropertyAttributes attrs = p->GetOwnPropertyAttributes(key);
            if (p->HasNamedProperty(key) && !(attrs & PropertyAttributes::Writable)) {
                if (is_private || (Interpreter::Current() && Interpreter::Current()->IsStrictMode())) {
                    if (Interpreter::Current()) {
                        Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                            new JSError("TypeError", "Cannot assign to read only property")));
                    }
                }
                return;
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
                    new JSError("TypeError", "Cannot add property to a non-extensible object")));
            }
            return;
        }
        if (own_attributes_.find(std::string(key)) == own_attributes_.end()) {
            own_attributes_[std::string(key)] = PropertyAttributes::Default;
        }
        SetFastSlot(desc->slot_index, desc->is_in_object, value);
        return;
    }
    if (is_sealed_ || !is_extensible_) {
        if (Interpreter::Current() && Interpreter::Current()->IsStrictMode()) {
            Interpreter::Current()->SetPendingException(TaggedValue::FromHeapObject(
                new JSError("TypeError", "Cannot add property to a non-extensible object")));
        }
        return;
    }

    // Slow path: Property does not exist; transition shape
    Shape* next_shape = current_shape->FindOrCreateTransition(key, next_shape_id++, current_shape->Prototype());
    SetShape(next_shape);

    const PropertyDescriptor* new_desc = next_shape->LookupProperty(key);
    assert(new_desc != nullptr);
    if (own_attributes_.find(std::string(key)) == own_attributes_.end()) {
        own_attributes_[std::string(key)] = PropertyAttributes::Default;
    }
    SetFastSlot(new_desc->slot_index, new_desc->is_in_object, value);
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
        });
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
        });
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
        });
    } else if (state_ == PromiseState::Rejected) {
        TaggedValue reason = result_;
        queue.Enqueue([on_rejected, reason, downstream, &queue]() {
            if (!on_rejected.IsEmpty() && !on_rejected.IsUndefined()) {
                TaggedValue res = CallFunction(on_rejected, TaggedValue::Undefined(), {reason});
                if (downstream) downstream->Fulfill(res, queue);
            } else {
                if (downstream) downstream->Reject(reason, queue);
            }
        });
    } else {
        reactions_.push_back({on_fulfilled, on_rejected, downstream});
    }
}

// JSRegExp Implementation
bool JSRegExp::Test(std::string_view str) const {
    try {
        std::regex::flag_type flags = std::regex::ECMAScript;
        if (flags_.find('i') != std::string::npos) {
            flags |= std::regex::icase;
        }
        std::regex re(pattern_, flags);
        std::string s(str);
        return std::regex_search(s, re);
    } catch (...) {
        return false;
    }
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
}

JSFunction::JSFunction(std::string_view name, std::shared_ptr<const BytecodeArray> bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(name),
      bytecode_shared_(std::move(bytecode)),
      bytecode_(bytecode_shared_.get()),
      environment_(env) {
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSFunction::JSFunction(std::string_view name, const BytecodeArray* bytecode, LexicalEnvironment* env, Shape* shape)
    : JSObject(HeapObjectType::JSFunction, shape ? shape : (Realm::Current() ? Realm::Current()->GetRootShape() : nullptr)),
      name_(name),
      bytecode_(bytecode),
      environment_(env) {
    if (Realm::Current() && Realm::Current()->GetFunctionPrototype()) {
        SetPrototype(Realm::Current()->GetFunctionPrototype());
    }
}

JSObject* JSFunction::GetOrCreateConstructorPrototype() {
    if (ctor_prototype_.IsEmpty()) {
        Shape* root_shape = Realm::Current() ? Realm::Current()->GetRootShape() : nullptr;
        auto* proto = new JSObject(root_shape);
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
      name_(name),
      message_(message) {
    uint32_t sid = 8000;
    SetNamedProperty("name", TaggedValue::FromHeapObject(new JSString(name_)), sid);
    SetNamedProperty("message", TaggedValue::FromHeapObject(new JSString(message_)), sid);
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

} // namespace aether
