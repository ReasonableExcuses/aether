#ifndef AETHER_HEAP_H
#define AETHER_HEAP_H

#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/gc.h"
#include "aether/memory_budget.h"
#include <deque>
#include <map>
#include <thread>
#include <vector>
#include <unordered_set>
#include <functional>
#include <iostream>

namespace aether {

class IsolateHeap;
class HandleScope;
class ConcurrentMarker;

// Protect raw native temporaries, constructor arguments and partially built
// objects. Exiting does NOT collect: publish/root the result before a safepoint.
class NoGCScope {
public:
    explicit NoGCScope(IsolateHeap& heap);
    explicit NoGCScope(IsolateHeap* heap = nullptr);
    ~NoGCScope();
    NoGCScope(const NoGCScope&) = delete;
    NoGCScope& operator=(const NoGCScope&) = delete;
private:
    IsolateHeap* heap_;
};

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
    HandleScope(const HandleScope&) = delete;
    HandleScope& operator=(const HandleScope&) = delete;

    template <typename T>
    Handle<T> CreateHandle(T* obj) {
        TaggedValue val = obj ? TaggedValue::FromHeapObject(obj) : TaggedValue::Null();
        slots_.push_back(val);
        return Handle<T>(&slots_.back());
    }

    Handle<HeapObject> CreateHandle(TaggedValue val) {
        slots_.push_back(val);
        return Handle<HeapObject>(&slots_.back());
    }

    std::deque<TaggedValue>& Slots() noexcept { return slots_; }
    HandleScope* PrevScope() const noexcept { return prev_scope_; }

private:
    friend class IsolateHeap;
    IsolateHeap& heap_;
    std::deque<TaggedValue> slots_;
    HandleScope* prev_scope_{nullptr};
};

/**
 * @brief Managed 4GB Isolate Heap orchestrating Nursery, Immix, and GC passes.
 */
class IsolateHeap {
public:
    explicit IsolateHeap(size_t nursery_size = kDefaultNurserySemiSpaceSize);
    ~IsolateHeap();

    // Precise managed allocation. Root all live values across allocation. For
    // opaque constructor/native arguments use NoGCScope. Constructors always
    // run with GC inhibited. Only argument-free/non-pointer scalar allocation
    // may automatically move the nursery; other allocation pressure uses STW
    // nonmoving MajorGC, with direct object/TaggedValue arguments rooted.
    template <typename T, typename... Args>
    T* Allocate(Args&&... args) {
        return AllocateImpl<T>(false, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    T* AllocateOld(Args&&... args) {
        return AllocateImpl<T>(true, std::forward<Args>(args)...);
    }

    template <typename T, typename... Args>
    T* AllocateStable(Args&&... args) {
        NoGCScope guard(*this);
        return AllocateOld<T>(std::forward<Args>(args)...);
    }

    // Generational Write Barrier
    AETHER_ALWAYS_INLINE void WriteBarrier(HeapObject* source, TaggedValue value) noexcept {
        if (source != nullptr && source->IsOld() && value.IsHeapObject()) {
            HeapObject* target = value.AsHeapObject();
            if (target->IsYoung()) {
                card_table_.MarkCard(source);
            }
        }
        NotifyConcurrentWrite(source, value);
    }

    void NotifyConcurrentWrite(HeapObject* source, TaggedValue value) noexcept;

    // GC Passes
    void MinorGC(); // Cheney Scavenge
    void MajorGC(); // Full Mark-Sweep
    void StartConcurrentMark();
    void FinalizeConcurrentGC();
    // Owning thread only, all native temporaries published/rooted; nonmoving.
    // Returns false when inside NoGCScope. No deferred collection on scope exit.
    bool CollectAtSafepoint();
    bool GCAllowed() const noexcept { return no_gc_depth_ == 0 && !collecting_; }
    void AssertOwnerThread() const;

    static IsolateHeap* Current() noexcept;
    static void SetCurrent(IsolateHeap* heap) noexcept;
    // Thread-owned fallback heap (collectible at explicit safe boundaries and
    // destroyed at thread exit). Prefer an explicit IsolateScope in embedders.
    // Roots belong to the allocation heap; moving a value to another isolate
    // does not transfer ownership or establish a root in this fallback heap.
    static IsolateHeap& RuntimeFallbackHeap();

    // Root management
    void PushHandleScope(HandleScope* scope) noexcept {
        scope->prev_scope_ = top_scope_;
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
    ConcurrentMarker* GetConcurrentMarker() noexcept;

    size_t MinorGCCount() const noexcept { return minor_gc_count_; }
    size_t MajorGCCount() const noexcept { return major_gc_count_; }

    // Heap budget and pressure
    size_t TotalAllocatedBytes() const noexcept { return budget_->ChargedBytes(); }
    size_t ManagedBytes() const noexcept { return budget_->ManagedBytes(); }
    size_t HeapBudget() const noexcept { return budget_->TotalLimit(); }
    void SetHeapBudget(size_t budget) noexcept { budget_->SetTotalLimit(budget); }
    std::shared_ptr<MemoryBudgetState> BudgetState() const noexcept { return budget_; }
    double HeapPressure() const noexcept {
        return HeapBudget() > 0 ? static_cast<double>(TotalAllocatedBytes()) / static_cast<double>(HeapBudget()) * 100.0 : 0.0;
    }

    size_t PendingFinalizationCount() const noexcept { return finalizers_.size(); }
    // Called by the owning Agent outside sweep. Values stay rooted for the
    // entire call; the sink must install its own roots if scheduling later.
    size_t DrainFinalizationQueue(const std::function<void(TaggedValue, TaggedValue)>& sink);

    // Mark-only STW entry used by the compatibility ConcurrentMarker facade.
    size_t MarkFromRoots(const std::vector<HeapObject*>& roots, bool process_weak = false);

private:
    friend class NoGCScope;
    template <typename A>
    void RootArgument(HandleScope& roots, const A& arg) {
        using U = std::remove_cvref_t<A>;
        if constexpr (std::is_same_v<U, TaggedValue>) roots.CreateHandle(arg);
        else if constexpr (std::is_pointer_v<U>) {
            if constexpr (std::is_base_of_v<HeapObject, std::remove_pointer_t<U>>) {
                if (arg) roots.CreateHandle(TaggedValue::FromHeapObject(arg));
            }
        } else if constexpr (std::is_same_v<U, std::vector<TaggedValue>>) {
            for (auto value : arg) roots.CreateHandle(value);
        }
    }

    template <typename T, typename... Args>
    T* AllocateImpl(bool old, Args&&... args) {
        static_assert(std::is_base_of_v<HeapObject, T>);
        static_assert(alignof(T) <= kObjectAlignment, "over-aligned heap object");
        AssertOwnerThread();
        HandleScope argument_roots(*this);
        (RootArgument(argument_roots, args), ...);
        constexpr size_t bytes = (sizeof(T) + kObjectAlignment - 1) & ~(kObjectAlignment - 1);
        // Even a scalar lvalue can refer into a rooted object that moves.
        constexpr bool movable_args = ((!std::is_lvalue_reference_v<Args> &&
            (std::is_arithmetic_v<std::remove_cvref_t<Args>> ||
            std::is_same_v<std::remove_cvref_t<Args>, std::nullptr_t> ||
            std::is_same_v<std::remove_cvref_t<Args>, Shape*>)) && ...);
        ReserveManaged(bytes);
        void* mem = nullptr;
        T* obj = nullptr;
        try {
            if (!old) {
                mem = nursery_.Allocate(bytes);
                if (!mem && GCAllowed() && movable_args) {
                    MinorGC();
                    mem = nursery_.Allocate(bytes);
                }
            }
            if (!mem) { old = true; mem = old_gen_.Allocate(bytes); }
            if (!mem) throw std::bad_alloc();
            NoGCScope construction(*this);
            // Member MemoryCharges must capture THIS heap even when callers
            // invoke a heap directly while another isolate is current.
            IsolateHeap* previous = Current();
            SetCurrent(this);
            try { obj = new (mem) T(std::forward<Args>(args)...); }
            catch (...) { SetCurrent(previous); throw; }
            SetCurrent(previous);
            obj->SetOld(old);
            if (old) old_gen_.RegisterObject(obj, true);
            else young_.emplace(obj, bytes);
            return obj;
        } catch (...) {
            if (obj) static_cast<HeapObject*>(obj)->~HeapObject();
            if (old && mem) old_gen_.DiscardAllocation(mem);
            budget_->ReleaseManaged(bytes);
            throw;
        }
    }
    void ReserveManaged(size_t bytes);
    void VisitRoots(const std::function<void(TaggedValue&)>& visitor);
    std::vector<HeapObject*> Trace(const std::vector<HeapObject*>& roots);
    void ProcessWeak(const std::vector<HeapObject*>& live,
                     const std::function<HeapObject*(HeapObject*)>& resolve);

    std::shared_ptr<MemoryBudgetState> budget_{std::make_shared<MemoryBudgetState>()};
    IsolateHeap* previous_current_heap_{nullptr};
    NurserySpace nursery_;
    ImmixSpace old_gen_;
    CardTable card_table_;
    std::unique_ptr<ConcurrentMarker> concurrent_marker_;

    HandleScope* top_scope_{nullptr};
    std::vector<TaggedValue*> extra_roots_;
    std::map<HeapObject*, size_t> young_;
    struct Finalization { TaggedValue callback; TaggedValue held_value; };
    std::deque<Finalization> finalizers_;

    size_t minor_gc_count_{0};
    size_t major_gc_count_{0};
    size_t no_gc_depth_{0};
    bool collecting_{false};
    bool draining_finalizers_{false};
    const std::thread::id owner_thread_{std::this_thread::get_id()};
};

inline HandleScope::HandleScope(IsolateHeap& heap)
    : heap_(heap), prev_scope_(nullptr) {
    heap_.AssertOwnerThread();
    heap_.PushHandleScope(this);
}

inline HandleScope::~HandleScope() {
    heap_.PopHandleScope(this);
}

/**
 * @brief RAII Scope for managing active IsolateHeap context switches across threads/agents.
 */
class IsolateScope {
public:
    explicit IsolateScope(IsolateHeap* heap)
        : prev_heap_(IsolateHeap::Current()) {
        IsolateHeap::SetCurrent(heap);
    }
    explicit IsolateScope(IsolateHeap& heap)
        : IsolateScope(&heap) {}
    ~IsolateScope() {
        IsolateHeap::SetCurrent(prev_heap_);
    }
    IsolateScope(const IsolateScope&) = delete;
    IsolateScope& operator=(const IsolateScope&) = delete;

private:
    IsolateHeap* prev_heap_;
};

using Heap = IsolateHeap;

/**
 * @brief Enterprise-grade allocation facade.
 *
 * Stable, owned runtime allocation. Never collects inside this operation, since
 * native expressions can hold unpublished raw pointers. Objects are NOT roots:
 * install handles/extra roots and call CollectAtSafepoint at a safe boundary.
 * Long-lived native captures need explicit roots for as long as they are used.
 */
template <typename T, typename... Args>
inline T* AllocateRuntime(Args&&... args) {
    if (IsolateHeap* h = IsolateHeap::Current()) {
        return h->AllocateStable<T>(std::forward<Args>(args)...);
    }
    return IsolateHeap::RuntimeFallbackHeap().AllocateStable<T>(std::forward<Args>(args)...);
}

} // namespace aether

#endif // AETHER_HEAP_H
