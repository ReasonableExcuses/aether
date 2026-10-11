#include "aether/heap_object.h"
#include "aether/runtime.h"
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
    if (IsArray()) {
        if (key == "length") return true;
        if (IsAllDigits(key)) {
            size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
            return idx < static_cast<const JSArray*>(this)->Length();
        }
    } else if (IsMap()) {
        if (key == "size") return true;
    } else if (IsSet()) {
        if (key == "size") return true;
    }

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

        TaggedValue proto = shape->Prototype();
        while (proto.IsHeapObject()) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
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
    if (IsArray() && IsAllDigits(key)) {
        size_t idx = static_cast<size_t>(std::stoull(std::string(key)));
        if (idx < static_cast<JSArray*>(this)->Length()) {
            static_cast<JSArray*>(this)->SetElement(idx, TaggedValue::Undefined());
            return true;
        }
    }
    Shape* shape = GetShape();
    if (shape != nullptr) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            SetFastSlot(desc->slot_index, desc->is_in_object, TaggedValue::Empty());
            return true;
        }
    }
    return true;
}

std::vector<std::string> JSObject::GetOwnPropertyNames() const {
    std::vector<std::string> names;
    if (IsArray()) {
        const auto* arr = static_cast<const JSArray*>(this);
        for (size_t i = 0; i < arr->Length(); ++i) {
            names.push_back(std::to_string(i));
        }
        names.push_back("length");
        return names;
    }
    Shape* shape = GetShape();
    if (shape != nullptr) {
        for (const auto& [name, desc] : shape->GetPropertyMap()) {
            TaggedValue val = GetFastSlot(desc.slot_index, desc.is_in_object);
            if (!val.IsEmpty()) {
                names.push_back(name);
            }
        }
    }
    return names;
}

void JSObject::SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id) {
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

    Shape* current_shape = GetShape();
    if (current_shape == nullptr && Realm::Current()) {
        current_shape = Realm::Current()->GetRootShape();
        SetShape(current_shape);
    }
    assert(current_shape != nullptr && "Object must have a valid shape");

    const PropertyDescriptor* desc = current_shape->LookupProperty(key);
    if (desc != nullptr) {
        // Fast path: Shape already contains property; overwrite existing slot
        SetFastSlot(desc->slot_index, desc->is_in_object, value);
        return;
    }

    // Slow path: Property does not exist; transition shape
    Shape* next_shape = current_shape->FindOrCreateTransition(key, next_shape_id++, current_shape->Prototype());
    SetShape(next_shape);

    const PropertyDescriptor* new_desc = next_shape->LookupProperty(key);
    assert(new_desc != nullptr);
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

} // namespace aether
