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
    for (auto& [obj, bytes] : young_) {
        obj->~HeapObject();
        budget_->ReleaseManaged(bytes);
    }
    young_.clear();
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

void IsolateHeap::VisitRoots(const std::function<void(TaggedValue&)>& visitor) {
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
    old_gen_.ClearMarks();
    for (auto& [obj, bytes] : young_) obj->SetMarked(false);
    std::unordered_set<HeapObject*> seen;
    std::vector<HeapObject*> live;
    auto mark = [&](HeapObject* obj) {
        if (obj && seen.insert(obj).second) {
            live.push_back(obj);
            obj->SetMarked(true);
        }
    };
    auto visit = [&](TaggedValue& slot) { if (slot.IsHeapObject()) mark(slot.AsHeapObject()); };
    VisitRoots(visit);
    for (auto* root : roots) mark(root);
    size_t scanned = 0;
    for (;;) {
        while (scanned < live.size()) {
            HeapObject* obj = live[scanned++];
            old_gen_.MarkObject(obj);
            obj->IteratePointers(visit);
        }
        const size_t before = live.size();
        // Repeat until no ephemeron adds a strong edge, including newly
        // discovered maps and chains whose keys become reachable via values.
        for (size_t i = 0; i < live.size(); ++i) {
            if (live[i]->IsWeakMap()) {
                static_cast<JSWeakMap*>(live[i])->TraceEphemerons(
                    [&](HeapObject* key) { return seen.contains(key); }, visit);
            }
        }
        if (live.size() == before) break;
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
    std::unordered_set<HeapObject*> alive(live.begin(), live.end());
    ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
        return alive.contains(obj) ? obj : nullptr;
    });

    struct Move { HeapObject* source; HeapObject* destination; bool old; };
    std::vector<Move> moves;
    moves.reserve(young_.size());
    std::map<HeapObject*, HeapObject*> forwarding;
    std::map<HeapObject*, size_t> survivors;
    uintptr_t bump = nursery_.ToSpaceStart();
    // Reserve all destinations and metadata before the first move. OS OOM
    // leaves the original graph intact. RelocateTo must be a nonthrowing move.
    try {
        for (auto& [obj, bytes] : young_) {
            if (!alive.contains(obj)) continue;
            bool old = obj->GetAge() + 1 >= kTenuringAgeThreshold;
            auto* destination = old ? static_cast<HeapObject*>(old_gen_.Allocate(bytes))
                : reinterpret_cast<HeapObject*>(bump);
            moves.push_back({obj, destination, old});
            forwarding.emplace(obj, destination);
            if (!old) {
                assert(bytes <= nursery_.ToSpaceLimit() - bump);
                bump += bytes;
                survivors.emplace(destination, bytes);
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
    std::function<void(TaggedValue&)> relocate = [&](TaggedValue& slot) {
        if (slot.IsHeapObject()) slot = TaggedValue::FromHeapObject(resolve(slot.AsHeapObject()));
    };
    std::function<HeapObject*(HeapObject*)> resolve_weak = resolve;
    std::function<bool(HeapObject*)> all_keys_live = [](HeapObject*) { return true; };
    // All core nursery RelocateTo implementations move noexcept C++ members.
    // A custom object violating this contract cannot be safely recovered after
    // a partial move; fail closed rather than expose an inconsistent heap.
    try {
        for (auto& move : moves) {
            HeapObject* moved = move.source->RelocateTo(move.destination);
            assert(moved == move.destination);
            moved->IncrementAge();
            moved->SetOld(move.old);
            if (move.old) old_gen_.RegisterObject(moved, true);
        }
        VisitRoots(relocate);
        for (auto*& obj : live) obj = resolve(obj);
        for (auto* obj : live) {
            // Includes promoted survivors: their children must be relocated.
            obj->IteratePointers(relocate);
            if (obj->IsWeakMap()) {
                static_cast<JSWeakMap*>(obj)->TraceEphemerons(all_keys_live, relocate);
            }
        }
        ProcessWeak(live, resolve_weak);
        WasmRegistryRelocate([&](JSObject* obj) -> JSObject* {
            return static_cast<JSObject*>(resolve(obj));
        });
        WasmRegistryPruneDead([&](JSObject* obj) { return alive.contains(obj); });
    } catch (...) {
        std::terminate();
    }
    for (auto& [obj, bytes] : young_) {
        obj->~HeapObject(); // also destroys the moved-from C++ object
        if (!forwarding.contains(obj)) budget_->ReleaseManaged(bytes);
    }
    young_.swap(survivors);
    nursery_.Flip(bump);
    card_table_.Clear();
    ++minor_gc_count_;
    HeapVerifier::VerifyAfterGC(*this);
    if (Options::Get().trace_gc) {
        std::cout << "[GC] MinorGC #" << minor_gc_count_ << ": live young=" << survivors.size() << std::endl;
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
    std::unordered_set<HeapObject*> alive(live.begin(), live.end());
    // Membership, not poisoned mark bits, decides weak liveness. All weak
    // processing and cleanup queue allocation precedes any reclamation.
    ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
        return alive.contains(obj) ? obj : nullptr;
    });
    WasmRegistryPruneDead([&](JSObject* obj) { return alive.contains(obj); });
    for (auto it = young_.begin(); it != young_.end(); ) {
        if (alive.contains(it->first)) { ++it; continue; }
        HeapObject* obj = it->first;
        size_t bytes = it->second;
        obj->~HeapObject();
        std::memset(static_cast<void*>(obj), kNurseryPoisonByte, bytes);
        budget_->ReleaseManaged(bytes);
        it = young_.erase(it);
    }
    if (young_.empty()) nursery_.ResetBumpPointer();
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
        std::unordered_set<HeapObject*> alive(live.begin(), live.end());
        ProcessWeak(live, [&](HeapObject* obj) -> HeapObject* {
            return alive.contains(obj) ? obj : nullptr;
        });
    }
    return live.size();
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
