#include "aether/heap_object.h"

namespace aether {

TaggedValue JSObject::GetNamedProperty(std::string_view key) const {
    Shape* shape = GetShape();
    if (AETHER_LIKELY(shape != nullptr)) {
        const PropertyDescriptor* desc = shape->LookupProperty(key);
        if (desc != nullptr) {
            return GetFastSlot(desc->slot_index, desc->is_in_object);
        }

        // Walk prototype chain
        TaggedValue proto = shape->Prototype();
        while (proto.IsHeapObject()) {
            HeapObject* p_obj = proto.AsHeapObject();
            if (p_obj->IsJSObject()) {
                JSObject* js_proto = static_cast<JSObject*>(p_obj);
                Shape* p_shape = js_proto->GetShape();
                if (p_shape != nullptr) {
                    const PropertyDescriptor* p_desc = p_shape->LookupProperty(key);
                    if (p_desc != nullptr) {
                        return js_proto->GetFastSlot(p_desc->slot_index, p_desc->is_in_object);
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

void JSObject::SetNamedProperty(std::string_view key, TaggedValue value, uint32_t& next_shape_id) {
    Shape* current_shape = GetShape();
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

} // namespace aether
