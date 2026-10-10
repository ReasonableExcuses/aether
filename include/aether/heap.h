#ifndef AETHER_HEAP_H
#define AETHER_HEAP_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/gc.h"
#include <vector>
#include <functional>
#include <iostream>

namespace aether {

class IsolateHeap;
class HandleScope;

template <typename T>
class Handle {
public:
    Handle() : slot_(nullptr) {}
    explicit Handle(TaggedValue* slot) : slot_(slot) {}

    T* operator->() const noexcept {
        assert(slot_ != nullptr && slot_->IsHeapObject());
        return slot_->As<T>();
    }

    T* Get() const noexcept {
        if (!slot_ || !slot_->IsHeapObject()) return nullptr;
        return slot_->As<T>();
    }

    TaggedValue GetTagged() const noexcept {
        return slot_ ? *slot_ : TaggedValue::Undefined();
    }

    TaggedValue* Slot() const noexcept { return slot_; }

private:
    TaggedValue* slot_;
};

/**
 * @brief RAII HandleScope registering root variables for precise garbage collection.
 */
class HandleScope {
public:
    explicit HandleScope(IsolateHeap& heap);
    ~HandleScope();

    template <typename T>
    Handle<T> CreateHandle(T* obj) {
        TaggedValue val = TaggedValue::FromHeapObject(obj);
        slots_.push_back(val);
        return Handle<T>(&slots_.back());
    }

    Handle<HeapObject> CreateHandle(TaggedValue val) {
        slots_.push_back(val);
        return Handle<HeapObject>(&slots_.back());
    }

    std::vector<TaggedValue>& Slots() noexcept { return slots_; }
    HandleScope* PrevScope() const noexcept { return prev_scope_; }

private:
    IsolateHeap& heap_;
    std::vector<TaggedValue> slots_;
    HandleScope* prev_scope_{nullptr};
};

/**
 * @brief Managed 4GB Isolate Heap orchestrating Nursery, Immix, and GC passes.
 */
class IsolateHeap {
public:
    explicit IsolateHeap(size_t nursery_size = kDefaultNurserySemiSpaceSize);
    ~IsolateHeap();

    // Allocation interface
    template <typename T, typename... Args>
    T* Allocate(Args&&... args) {
        size_t bytes = sizeof(T);
        // Align to 8 bytes
        bytes = (bytes + kObjectAlignment - 1) & ~(kObjectAlignment - 1);

        void* mem = nursery_.Allocate(bytes);
        if (AETHER_UNLIKELY(!mem)) {
            // Nursery exhausted: Trigger Minor GC
            MinorGC();
            mem = nursery_.Allocate(bytes);
            if (AETHER_UNLIKELY(!mem)) {
                // Nursery still full, allocate directly in Old Generation
                mem = old_gen_.Allocate(bytes);
                T* obj = new (mem) T(std::forward<Args>(args)...);
                obj->SetOld(true);
                return obj;
            }
        }

        T* obj = new (mem) T(std::forward<Args>(args)...);
        obj->SetOld(false);
        return obj;
    }

    // Generational Write Barrier
    AETHER_ALWAYS_INLINE void WriteBarrier(HeapObject* source, TaggedValue value) noexcept {
        if (source != nullptr && source->IsOld() && value.IsHeapObject()) {
            HeapObject* target = value.AsHeapObject();
            if (target->IsYoung()) {
                card_table_.MarkCard(source);
            }
        }
    }

    // GC Passes
    void MinorGC(); // Cheney Scavenge
    void MajorGC(); // Full Mark-Sweep

    // Root management
    void PushHandleScope(HandleScope* scope) noexcept {
        top_scope_ = scope;
    }

    void PopHandleScope(HandleScope* scope) noexcept {
        assert(top_scope_ == scope);
        top_scope_ = scope->PrevScope();
    }

    void RegisterExtraRoot(TaggedValue* root) {
        extra_roots_.push_back(root);
    }

    void UnregisterExtraRoot(TaggedValue* root) {
        for (auto it = extra_roots_.begin(); it != extra_roots_.end(); ++it) {
            if (*it == root) {
                extra_roots_.erase(it);
                break;
            }
        }
    }

    // Queries
    NurserySpace& GetNursery() noexcept { return nursery_; }
    ImmixSpace& GetOldGen() noexcept { return old_gen_; }
    CardTable& GetCardTable() noexcept { return card_table_; }

    size_t MinorGCCount() const noexcept { return minor_gc_count_; }
    size_t MajorGCCount() const noexcept { return major_gc_count_; }

private:
    void EvacuateObject(TaggedValue& slot);

    NurserySpace nursery_;
    ImmixSpace old_gen_;
    CardTable card_table_;

    HandleScope* top_scope_{nullptr};
    std::vector<TaggedValue*> extra_roots_;

    size_t minor_gc_count_{0};
    size_t major_gc_count_{0};
};

inline HandleScope::HandleScope(IsolateHeap& heap)
    : heap_(heap), prev_scope_(nullptr) {
    heap_.PushHandleScope(this);
}

inline HandleScope::~HandleScope() {
    heap_.PopHandleScope(this);
}

using Heap = IsolateHeap;

} // namespace aether

#endif // AETHER_HEAP_H
