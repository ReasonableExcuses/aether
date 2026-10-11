#include "aether/heap.h"
#include <deque>

namespace aether {

static thread_local IsolateHeap* s_current_heap_{nullptr};

IsolateHeap* IsolateHeap::Current() noexcept { return s_current_heap_; }
void IsolateHeap::SetCurrent(IsolateHeap* heap) noexcept { s_current_heap_ = heap; }

void RecordWriteBarrier(HeapObject* source, TaggedValue value) {
    if (s_current_heap_) {
        s_current_heap_->WriteBarrier(source, value);
    }
}

IsolateHeap::IsolateHeap(size_t nursery_size)
    : nursery_(nursery_size),
      old_gen_(),
      card_table_(nursery_.FromSpaceStart(), kHeapReservationSize) {
    if (!s_current_heap_) {
        s_current_heap_ = this;
    }
    if (nursery_.HasReservation()) {
        old_gen_.SetReservation(nursery_.OldGenStart(), kHeapReservationSize - (nursery_size * 2));
    }
}

IsolateHeap::~IsolateHeap() {
    if (s_current_heap_ == this) {
        s_current_heap_ = nullptr;
    }
}

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
        HeapObject* tenured = obj->RelocateTo(old_mem);
        tenured->SetOld(true);
        obj->SetForwardedAddress(tenured);
        slot = TaggedValue::FromHeapObject(tenured);
    } else {
        // Evacuate object into Nursery To-space
        uintptr_t to_limit = nursery_.ToSpaceLimit();

        if (to_space_bump_ + obj_size <= to_limit) {
            void* to_mem = reinterpret_cast<void*>(to_space_bump_);
            to_space_bump_ += obj_size;
            HeapObject* evacuated = obj->RelocateTo(to_mem);
            evacuated->SetOld(false);
            obj->SetForwardedAddress(evacuated);
            slot = TaggedValue::FromHeapObject(evacuated);
        } else {
            // To-space exhausted during scavenge, promote to Old Gen
            void* old_mem = old_gen_.Allocate(obj_size);
            HeapObject* tenured = obj->RelocateTo(old_mem);
            tenured->SetOld(true);
            obj->SetForwardedAddress(tenured);
            slot = TaggedValue::FromHeapObject(tenured);
        }
    }
}

void IsolateHeap::MinorGC() {
    to_space_bump_ = nursery_.ToSpaceStart();
    uintptr_t scan_ptr = to_space_bump_;

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

    // 3. Scan Remembered Set (Old-to-Young references recorded in CardTable)
    for (auto it = remembered_old_objects_.begin(); it != remembered_old_objects_.end(); ) {
        HeapObject* old_obj = *it;
        bool has_young_reference = false;
        old_obj->IteratePointers([this, &has_young_reference](TaggedValue& slot) {
            if (slot.IsHeapObject() && slot.AsHeapObject()->IsYoung()) {
                EvacuateObject(slot);
                if (slot.IsHeapObject() && slot.AsHeapObject()->IsYoung()) {
                    has_young_reference = true;
                }
            }
        });
        if (!has_young_reference) {
            it = remembered_old_objects_.erase(it);
        } else {
            ++it;
        }
    }

    // 4. Cheney-style breadth-first traversal in To-space
    while (scan_ptr < to_space_bump_) {
        HeapObject* obj = reinterpret_cast<HeapObject*>(scan_ptr);
        size_t size = obj->ObjectSize();
        if (size == 0) break;
        size = (size + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

        obj->IteratePointers([this](TaggedValue& child) {
            EvacuateObject(child);
        });

        scan_ptr += size;
    }

    // 5. Flip semi-spaces and re-synchronize card table with remembered set
    nursery_.Flip();
    card_table_.Clear();
    for (HeapObject* old_obj : remembered_old_objects_) {
        card_table_.MarkCard(old_obj);
    }
    minor_gc_count_++;
}

void IsolateHeap::MajorGC() {
    // 0. Reset marks on objects from previous GC cycle
    for (HeapObject* obj : previously_marked_objects_) {
        obj->SetMarked(false);
    }
    previously_marked_objects_.clear();

    std::vector<HeapObject*> mark_stack;
    std::vector<HeapObject*> marked_objects;

    // 1. Mark phase starting from HandleScope roots
    HandleScope* scope = top_scope_;
    while (scope != nullptr) {
        for (TaggedValue& root_val : scope->Slots()) {
            if (root_val.IsHeapObject()) {
                HeapObject* obj = root_val.AsHeapObject();
                if (!obj->IsMarked()) {
                    obj->SetMarked(true);
                    mark_stack.push_back(obj);
                    marked_objects.push_back(obj);
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
                marked_objects.push_back(obj);
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

        curr->IteratePointers([&mark_stack, &marked_objects](TaggedValue& child) {
            if (child.IsHeapObject()) {
                HeapObject* child_obj = child.AsHeapObject();
                if (!child_obj->IsMarked()) {
                    child_obj->SetMarked(true);
                    mark_stack.push_back(child_obj);
                    marked_objects.push_back(child_obj);
                }
            }
        });
    }

    // 2. Sweep phase in Old Generation Immix space
    old_gen_.Sweep();

    // 3. Save marked objects so their mark bits can be reset before the next Major GC cycle
    previously_marked_objects_ = std::move(marked_objects);

    major_gc_count_++;
}

} // namespace aether
