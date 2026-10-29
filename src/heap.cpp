#include "aether/heap.h"
#include "aether/concurrent_marker.h"
#include "aether/advanced_intrinsics.h"
#include "aether/wasm.h"
#include "aether/heap_verifier.h"
#include "aether/options.h"
#include <deque>
#include <cstring>
#include <stdexcept>

namespace aether {

static thread_local IsolateHeap* s_current_heap_{nullptr};

IsolateHeap* IsolateHeap::Current() noexcept { return s_current_heap_; }
void IsolateHeap::SetCurrent(IsolateHeap* heap) noexcept { s_current_heap_ = heap; }

std::shared_ptr<MemoryBudgetState> CurrentMemoryBudgetState() noexcept {
    return IsolateHeap::Current() ? IsolateHeap::Current()->BudgetState() : nullptr;
}

NoGCScope::NoGCScope(IsolateHeap& heap) : NoGCScope(&heap) {}
NoGCScope::NoGCScope(IsolateHeap* heap) : heap_(heap ? heap : IsolateHeap::Current()) {
    if (heap_) {
        heap_->AssertOwnerThread();
        ++heap_->no_gc_depth_;
    }
}
NoGCScope::~NoGCScope() { if (heap_) --heap_->no_gc_depth_; }

namespace {
struct CollectionScope {
    explicit CollectionScope(bool& active) : active(active) { active = true; }
    ~CollectionScope() { active = false; }
    bool& active;
};
}

void IsolateHeap::AssertOwnerThread() const {
    if (owner_thread_ != std::this_thread::get_id()) {
        throw std::logic_error("Isolate heap accessed from a non-owning thread");
    }
}

IsolateHeap& IsolateHeap::RuntimeFallbackHeap() {
    static thread_local std::unique_ptr<IsolateHeap> fallback;
    if (!fallback) {
        IsolateHeap* previous = Current();
        fallback = std::make_unique<IsolateHeap>();
        SetCurrent(previous);
    }
    return *fallback;
}

void RecordWriteBarrier(HeapObject* source, TaggedValue value) {
    if (s_current_heap_) {
        s_current_heap_->WriteBarrier(source, value);
    }
}

IsolateHeap::IsolateHeap(size_t nursery_size)
    : nursery_(nursery_size),
      old_gen_(),
      card_table_(nursery_.FromSpaceStart(), kHeapReservationSize) {
    old_gen_.SetBudget(budget_);
    if (nursery_.HasReservation()) {
        old_gen_.SetReservation(nursery_.OldGenStart(), kHeapReservationSize - (nursery_size * 2));
    }
    concurrent_marker_ = std::make_unique<ConcurrentMarker>(*this);
    if (Options::Get().max_heap_mb > 0) {
        budget_->SetTotalLimit(Options::Get().max_heap_mb * 1024 * 1024);
    }
    previous_current_heap_ = s_current_heap_;
    s_current_heap_ = this;
}

IsolateHeap::~IsolateHeap() {
    if (concurrent_marker_) {
        concurrent_marker_->Stop();
    }
    collecting_ = true;
    for (auto* obj : young_objects_) {
        obj->~HeapObject();
        if (obj->IsCharged() && budget_) {
            budget_->ReleaseManaged(obj->GetAllocatedSize());
        }
    }
    young_objects_.clear();
    old_gen_.DestroyAll();
    if (s_current_heap_ == this) {
        s_current_heap_ = previous_current_heap_;
    }
}

void IsolateHeap::ReserveManaged(size_t bytes) {
    if (collecting_) throw std::logic_error("Allocation during GC/destruction");
    if (budget_->TryReserveManaged(bytes)) return;
    // No moving collection here: constructor argument roots are snapshots of
    // raw parameters. Runtime/native allocation uses NoGCScope and must retry
    // only after publishing its temporaries at a safe boundary.
    if (GCAllowed()) {
        MajorGC();
        if (budget_->TryReserveManaged(bytes)) return;
    }
    throw std::length_error("Aether managed heap budget exceeded");
}

void IsolateHeap::VisitRoots(SlotVisitor* visitor) {
    if (!visitor) return;
    for (HandleScope* scope = top_scope_; scope; scope = scope->PrevScope()) {
        for (auto& value : scope->Slots()) visitor->Visit(&value);
    }
    for (auto* root : extra_roots_) if (root) visitor->Visit(root);
    for (auto& entry : finalizers_) {
        visitor->Visit(&entry.callback);
        visitor->Visit(&entry.held_value);
    }
}

void IsolateHeap::VisitRoots(const std::function<void(TaggedValue&)>& visitor) {
    if (!visitor) return;
    for (HandleScope* scope = top_scope_; scope; scope = scope->PrevScope()) {
        for (auto& value : scope->Slots()) visitor(value);
    }
    for (auto* root : extra_roots_) if (root) visitor(*root);
    for (auto& entry : finalizers_) {
        visitor(entry.callback);
        visitor(entry.held_value);
    }
}

std::vector<HeapObject*> IsolateHeap::Trace(const std::vector<HeapObject*>& roots) {
    auto mark_start = std::chrono::steady_clock::now();
    old_gen_.ClearMarks();
    for (auto* obj : young_objects_) obj->SetMarked(false);

    mark_stack_.clear();
    std::vector<HeapObject*> live;
    std::vector<JSWeakMap*> weak_maps;

    class MarkVisitor final : public SlotVisitor {
    public:
        MarkVisitor(std::vector<HeapObject*>& mark_stack,
                    std::vector<HeapObject*>& live,
                    std::vector<JSWeakMap*>& weak_maps,
                    ImmixSpace& old_gen)
            : mark_stack_(mark_stack), live_(live), weak_maps_(weak_maps), old_gen_(old_gen) {}

        void Visit(TaggedValue* slot) override {
            if (!slot) return;
            TaggedValue val = *slot;
            if (val.IsHeapObject()) {
                HeapObject* obj = val.AsHeapObject();
                if (obj && !obj->IsMarked()) {
                    obj->SetMarked(true);
                    if (obj->IsOld()) {
                        old_gen_.MarkObject(obj);
                    }
                    mark_stack_.push_back(obj);
                    live_.push_back(obj);
                    if (obj->IsWeakMap()) {
                        weak_maps_.push_back(static_cast<JSWeakMap*>(obj));
                    }
                }
            }
        }

    private:
        std::vector<HeapObject*>& mark_stack_;
        std::vector<HeapObject*>& live_;
        std::vector<JSWeakMap*>& weak_maps_;
        ImmixSpace& old_gen_;
    };

    MarkVisitor mark_visitor(mark_stack_, live, weak_maps, old_gen_);

    // 1. Visit roots
    VisitRoots(&mark_visitor);
    for (auto* root : roots) {
        if (root && !root->IsMarked()) {
            root->SetMarked(true);
            if (root->IsOld()) {
                old_gen_.MarkObject(root);
            }
            mark_stack_.push_back(root);
            live.push_back(root);
            if (root->IsWeakMap()) {
                weak_maps.push_back(static_cast<JSWeakMap*>(root));
            }
        }
    }

    // 2. Drain mark stack (DFS order)
    while (!mark_stack_.empty()) {
        HeapObject* obj = mark_stack_.back();
        mark_stack_.pop_back();
        obj->IteratePointers(&mark_visitor);
    }

    // 3. Ephemeron fixpoint loop
    for (;;) {
        const size_t before = live.size();
        for (auto* wm : weak_maps) {
            wm->TraceEphemerons(
                [](HeapObject* key) { return key && key->IsMarked(); },
                &mark_visitor);
        }
        while (!mark_stack_.empty()) {
            HeapObject* obj = mark_stack_.back();
            mark_stack_.pop_back();
            obj->IteratePointers(&mark_visitor);
        }
        if (live.size() == before) break;
    }

    if (Options::Get().trace_gc) {
        auto mark_end = std::chrono::steady_clock::now();
        double mark_ms = std::chrono::duration<double, std::milli>(mark_end - mark_start).count();
        std::cout << "[GC] Mark completed in " << mark_ms << "ms: " << live.size() << " live objects\n";
    }

    return live;
}

void IsolateHeap::ProcessWeak(const std::vector<HeapObject*>& live,
                             const std::function<HeapObject*(HeapObject*)>& resolve) {
    for (auto* obj : live) {
        if (obj->IsWeakRef()) {
            auto* weak = static_cast<JSWeakRef*>(obj);
            weak->SetTarget(resolve(weak->Target()));
        } else if (obj->IsWeakMap()) {
            static_cast<JSWeakMap*>(obj)->ResolveWeakKeys(resolve);
        } else if (obj->IsWeakSet()) {
            static_cast<JSWeakSet*>(obj)->ResolveWeakEntries(resolve);
        } else if (obj->IsFinalizationRegistry()) {
            static_cast<JSFinalizationRegistry*>(obj)->ProcessWeakTargets(resolve,
                [this](TaggedValue callback, TaggedValue held) {
                    finalizers_.push_back({callback, held});
                });
        }
    }
}

void IsolateHeap::MinorGC() {
    AssertOwnerThread();
    if (!GCAllowed()) return;
    HeapVerifier::VerifyBeforeGC(*this);
    CollectionScope collecting(collecting_);
    // Until every mutation path has a complete barrier, scan all tenured
    // objects. This is conservative for a minor cycle, not permanent rooting.
    auto live = Trace(old_gen_.Objects());
    ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
        return (obj && obj->IsMarked()) ? obj : nullptr;
    });

    struct Move { HeapObject* source; HeapObject* destination; bool old; };
    std::vector<Move> moves;
    moves.reserve(young_objects_.size());
    std::unordered_map<HeapObject*, HeapObject*> forwarding;
    std::vector<HeapObject*> survivors;
    survivors.reserve(young_objects_.size());
    uintptr_t bump = nursery_.ToSpaceStart();
    // Reserve all destinations and metadata before the first move. OS OOM
    // leaves the original graph intact. RelocateTo must be a nonthrowing move.
    try {
        for (auto* obj : young_objects_) {
            if (!obj->IsMarked()) continue;
            size_t bytes = obj->GetAllocatedSize();
            bool old = obj->GetAge() + 1 >= kTenuringAgeThreshold;
            auto* destination = old ? static_cast<HeapObject*>(old_gen_.Allocate(bytes))
                : reinterpret_cast<HeapObject*>(bump);
            moves.push_back({obj, destination, old});
            forwarding.emplace(obj, destination);
            if (!old) {
                assert(bytes <= nursery_.ToSpaceLimit() - bump);
                bump += bytes;
                survivors.push_back(destination);
            }
        }
    } catch (...) {
        for (auto& move : moves) if (move.old) old_gen_.DiscardAllocation(move.destination);
        throw;
    }
    auto resolve = [&](HeapObject* obj) -> HeapObject* {
        auto it = forwarding.find(obj);
        return it == forwarding.end() ? obj : it->second;
    };
    class RelocateVisitor final : public SlotVisitor {
    public:
        explicit RelocateVisitor(const std::unordered_map<HeapObject*, HeapObject*>& forwarding)
            : forwarding_(forwarding) {}

        void Visit(TaggedValue* slot) override {
            if (!slot) return;
            TaggedValue val = *slot;
            if (val.IsHeapObject()) {
                auto it = forwarding_.find(val.AsHeapObject());
                if (it != forwarding_.end()) {
                    *slot = TaggedValue::FromHeapObject(it->second);
                }
            }
        }
    private:
        const std::unordered_map<HeapObject*, HeapObject*>& forwarding_;
    };
    RelocateVisitor relocate(forwarding);
    std::function<HeapObject*(HeapObject*)> resolve_weak = resolve;
    std::function<bool(HeapObject*)> all_keys_live = [](HeapObject*) { return true; };
    // All core nursery RelocateTo implementations move noexcept C++ members.
    // A custom object violating this contract cannot be safely recovered after
    // a partial move; fail closed rather than expose an inconsistent heap.
    try {
        for (auto& move : moves) {
            HeapObject* moved = move.source->RelocateTo(move.destination);
            assert(moved == move.destination);
            moved->SetAllocatedSize(move.source->GetAllocatedSize());
            moved->SetCharged(move.source->IsCharged());
            moved->IncrementAge();
            moved->SetOld(move.old);
            if (move.old) old_gen_.RegisterObject(moved, moved->IsCharged());
        }
        VisitRoots(&relocate);
        for (auto*& obj : live) obj = resolve(obj);
        for (auto* obj : live) {
            // Includes promoted survivors: their children must be relocated.
            obj->IteratePointers(&relocate);
            if (obj->IsWeakMap()) {
                static_cast<JSWeakMap*>(obj)->TraceEphemerons(all_keys_live, &relocate);
            }
        }
        ProcessWeak(live, resolve_weak);
        WasmRegistryRelocate([&](JSObject* obj) -> JSObject* {
            return static_cast<JSObject*>(resolve(obj));
        });
        WasmRegistryPruneDead([&](JSObject* obj) { return obj && Contains(obj) && obj->IsMarked(); });
    } catch (...) {
        std::terminate();
    }
    for (auto* obj : young_objects_) {
        size_t bytes = obj->GetAllocatedSize();
        bool was_charged = obj->IsCharged();
        obj->~HeapObject(); // also destroys the moved-from C++ object
        if (!forwarding.contains(obj) && was_charged && budget_) budget_->ReleaseManaged(bytes);
    }
    young_objects_.swap(survivors);
    nursery_.Flip(bump);
    card_table_.Clear();
    ++minor_gc_count_;
    HeapVerifier::VerifyAfterGC(*this);
    if (Options::Get().trace_gc) {
        std::cout << "[GC] MinorGC #" << minor_gc_count_ << ": live young=" << young_objects_.size() << std::endl;
    }
}

void IsolateHeap::NotifyConcurrentWrite(HeapObject* source, TaggedValue value) noexcept {
    if (concurrent_marker_ && concurrent_marker_->IsActive()) {
        concurrent_marker_->NotifyWrite(source, value);
    }
}

ConcurrentMarker* IsolateHeap::GetConcurrentMarker() noexcept {
    return concurrent_marker_.get();
}

void IsolateHeap::StartConcurrentMark() {
    AssertOwnerThread();
    if (GCAllowed() && concurrent_marker_) concurrent_marker_->Start({});
}

void IsolateHeap::FinalizeConcurrentGC() {
    AssertOwnerThread();
    if (!GCAllowed()) return;
    // A fresh STW trace sees the current graph, not an obsolete snapshot.
    MajorGC();
    if (concurrent_marker_) concurrent_marker_->Stop();
}

void IsolateHeap::MajorGC() {
    AssertOwnerThread();
    if (!GCAllowed()) return;
    HeapVerifier::VerifyBeforeGC(*this);
    CollectionScope collecting(collecting_);
    auto live = Trace({});
    // Liveness is tracked directly in object mark bits.
    ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
        return (obj && obj->IsMarked()) ? obj : nullptr;
    });
    WasmRegistryPruneDead([&](JSObject* obj) { return obj && Contains(obj) && obj->IsMarked(); });
    std::vector<HeapObject*> surviving_young;
    surviving_young.reserve(young_objects_.size());
    for (auto* obj : young_objects_) {
        if (obj->IsMarked()) {
            surviving_young.push_back(obj);
            continue;
        }
        size_t bytes = obj->GetAllocatedSize();
        bool was_charged = obj->IsCharged();
        obj->~HeapObject();
        std::memset(static_cast<void*>(obj), kNurseryPoisonByte, bytes);
        if (was_charged && budget_) budget_->ReleaseManaged(bytes);
    }
    young_objects_.swap(surviving_young);
    if (young_objects_.empty()) nursery_.ResetBumpPointer();
    old_gen_.Sweep();
    card_table_.Clear();
    ++major_gc_count_;
    HeapVerifier::VerifyAfterGC(*this);
    if (Options::Get().trace_gc) {
        std::cout << "[GC] MajorGC #" << major_gc_count_ << ": live=" << live.size() << " objects, usage=" << (budget_->ChargedBytes() / 1024) << "KB" << std::endl;
    }
}

bool IsolateHeap::CollectAtSafepoint() {
    AssertOwnerThread();
    if (!GCAllowed()) return false;
    MajorGC();
    return true;
}

size_t IsolateHeap::MarkFromRoots(const std::vector<HeapObject*>& roots, bool process_weak) {
    AssertOwnerThread();
    if (!GCAllowed()) throw std::logic_error("Marking inside NoGCScope");
    CollectionScope collecting(collecting_);
    auto live = Trace(roots);
    if (process_weak) {
        ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
            return (obj && obj->IsMarked()) ? obj : nullptr;
        });
    }
    return live.size();
}

bool IsolateHeap::Contains(const void* ptr) const noexcept {
    if (!ptr) return false;
    uintptr_t addr = reinterpret_cast<uintptr_t>(ptr);
    if (addr >= nursery_.FromSpaceStart() && addr < nursery_.FromSpaceLimit()) {
        return true;
    }
    if (addr >= nursery_.ToSpaceStart() && addr < nursery_.ToSpaceLimit()) {
        return true;
    }
    return old_gen_.Contains(ptr);
}

size_t IsolateHeap::DrainFinalizationQueue(const std::function<void(TaggedValue, TaggedValue)>& sink) {
    AssertOwnerThread();
    if (collecting_ || draining_finalizers_ || !sink) return 0;
    CollectionScope draining(draining_finalizers_);
    const size_t count = finalizers_.size();
    for (size_t i = 0; i < count; ++i) {
        // Pop before invoking so a throwing sink cannot deliver a callback
        // twice. Scope roots protect both values across nested GC/callbacks.
        HandleScope roots(*this);
        auto callback = roots.CreateHandle(finalizers_.front().callback);
        auto held = roots.CreateHandle(finalizers_.front().held_value);
        finalizers_.pop_front();
        NoGCScope native_temporaries(*this);
        sink(callback.GetTagged(), held.GetTagged());
    }
    return count;
}

} // namespace aether
