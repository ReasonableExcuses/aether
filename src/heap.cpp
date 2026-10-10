#include "aether/heap.h"
#include <deque>

namespace aether {

IsolateHeap::IsolateHeap(size_t nursery_size)
    : nursery_(nursery_size),
      old_gen_(),
      card_table_(nursery_.FromSpaceStart(), kHeapReservationSize) {}

IsolateHeap::~IsolateHeap() = default;

void IsolateHeap::EvacuateObject(TaggedValue& slot) {
    if (!slot.IsHeapObject()) return;

    HeapObject* obj = slot.AsHeapObject();
    if (!nursery_.Contains(obj)) return; // Already tenured in Old Generation

    if (obj->IsForwarded()) {
        slot = TaggedValue::FromHeapObject(obj->GetForwardedAddress());
        return;
    }

    size_t obj_size = obj->ObjectSize();
    obj_size = (obj_size + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

    obj->IncrementAge();
    if (obj->GetAge() >= kTenuringAgeThreshold) {
        // Tenure object into Immix Old Generation
        void* old_mem = old_gen_.Allocate(obj_size);
        assert(old_mem != nullptr && "Old Generation out of memory");
        std::memcpy(old_mem, obj, obj_size);

        HeapObject* tenured = reinterpret_cast<HeapObject*>(old_mem);
        tenured->SetOld(true);
        obj->SetForwardedAddress(tenured);
        slot = TaggedValue::FromHeapObject(tenured);
    } else {
        // Evacuate object into Nursery To-space
        uintptr_t to_start = nursery_.ToSpaceStart();
        uintptr_t to_limit = nursery_.ToSpaceLimit();
        static uintptr_t s_to_bump = to_start;

        if (s_to_bump + obj_size <= to_limit) {
            void* to_mem = reinterpret_cast<void*>(s_to_bump);
            s_to_bump += obj_size;
            std::memcpy(to_mem, obj, obj_size);

            HeapObject* evacuated = reinterpret_cast<HeapObject*>(to_mem);
            evacuated->SetOld(false);
            obj->SetForwardedAddress(evacuated);
            slot = TaggedValue::FromHeapObject(evacuated);
        } else {
            // To-space exhausted during scavenge, promote to Old Gen
            void* old_mem = old_gen_.Allocate(obj_size);
            std::memcpy(old_mem, obj, obj_size);
            HeapObject* tenured = reinterpret_cast<HeapObject*>(old_mem);
            tenured->SetOld(true);
            obj->SetForwardedAddress(tenured);
            slot = TaggedValue::FromHeapObject(tenured);
        }
    }
}

void IsolateHeap::MinorGC() {
    uintptr_t scan_ptr = nursery_.ToSpaceStart();
    uintptr_t to_limit = nursery_.ToSpaceLimit();
    (void)to_limit;

    // 1. Evacuate roots from all active HandleScopes
    HandleScope* scope = top_scope_;
    while (scope != nullptr) {
        for (TaggedValue& root_val : scope->Slots()) {
            EvacuateObject(root_val);
        }
        scope = scope->PrevScope();
    }

    // 2. Evacuate extra roots
    for (TaggedValue* root_ptr : extra_roots_) {
        if (root_ptr) {
            EvacuateObject(*root_ptr);
        }
    }

    // 3. Cheney-style breadth-first traversal in To-space
    // Scan objects evacuated to To-space and evacuate their children
    while (scan_ptr < nursery_.ToSpaceLimit()) {
        HeapObject* obj = reinterpret_cast<HeapObject*>(scan_ptr);
        if (obj->GetShape() == nullptr && obj->GetType() == static_cast<HeapObjectType>(0)) {
            break; // Reached unallocated frontier
        }

        size_t size = obj->ObjectSize();
        if (size == 0) break;
        size = (size + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

        obj->IteratePointers([this](TaggedValue& child) {
            EvacuateObject(child);
        });

        scan_ptr += size;
    }

    // 4. Flip semi-spaces
    nursery_.Flip();
    card_table_.Clear();
    minor_gc_count_++;
}

void IsolateHeap::MajorGC() {
    std::vector<HeapObject*> mark_stack;

    // 1. Mark phase starting from HandleScope roots
    HandleScope* scope = top_scope_;
    while (scope != nullptr) {
        for (TaggedValue& root_val : scope->Slots()) {
            if (root_val.IsHeapObject()) {
                HeapObject* obj = root_val.AsHeapObject();
                if (!obj->IsMarked()) {
                    obj->SetMarked(true);
                    mark_stack.push_back(obj);
                }
            }
        }
        scope = scope->PrevScope();
    }

    for (TaggedValue* root_ptr : extra_roots_) {
        if (root_ptr && root_ptr->IsHeapObject()) {
            HeapObject* obj = root_ptr->AsHeapObject();
            if (!obj->IsMarked()) {
                obj->SetMarked(true);
                mark_stack.push_back(obj);
            }
        }
    }

    // Drain mark stack
    while (!mark_stack.empty()) {
        HeapObject* curr = mark_stack.back();
        mark_stack.pop_back();

        if (old_gen_.Contains(curr)) {
            old_gen_.MarkObject(curr);
        }

        curr->IteratePointers([&mark_stack](TaggedValue& child) {
            if (child.IsHeapObject()) {
                HeapObject* child_obj = child.AsHeapObject();
                if (!child_obj->IsMarked()) {
                    child_obj->SetMarked(true);
                    mark_stack.push_back(child_obj);
                }
            }
        });
    }

    // 2. Sweep phase in Old Generation Immix space
    old_gen_.Sweep();
    major_gc_count_++;
}

} // namespace aether
