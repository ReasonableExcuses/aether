#include "aether/heap.h"
#include "aether/interpreter.h"
#include "aether/concurrent_marker.h"
#include <algorithm>
#include <setjmp.h>

#if defined(__has_feature)
#if __has_feature(address_sanitizer)
#define AETHER_ASAN_ACTIVE 1
#endif
#endif
#if defined(__SANITIZE_ADDRESS__)
#define AETHER_ASAN_ACTIVE 1
#endif

#if defined(AETHER_ASAN_ACTIVE) && (defined(__GNUC__) || defined(__clang__))
#if defined(__has_include)
#if __has_include(<sanitizer/asan_interface.h>)
#include <sanitizer/asan_interface.h>
#define AETHER_HAS_ASAN_INTERFACE 1
#endif
#endif
#endif

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#elif defined(__APPLE__)
#include <pthread.h>
#elif defined(__linux__)
#if !defined(_GNU_SOURCE)
#define _GNU_SOURCE
#endif
#include <pthread.h>
#endif
#include "aether/advanced_intrinsics.h"
#include "aether/wasm.h"
#include "aether/heap_verifier.h"
#include "aether/options.h"
#include <deque>
#include <cstring>
#include <stdexcept>

namespace aether {

#if defined(_WIN32)
[[maybe_unused]] static void* DetectThreadStackBase() {
    auto* tib = reinterpret_cast<NT_TIB*>(NtCurrentTeb());
    return tib ? tib->StackBase : nullptr;
}
#elif defined(__APPLE__)
[[maybe_unused]] static void* DetectThreadStackBase() {
    return pthread_get_stackaddr_np(pthread_self());
}
#elif defined(__linux__)
[[maybe_unused]] static void* DetectThreadStackBase() {
    pthread_attr_t attr;
    if (pthread_getattr_np(pthread_self(), &attr) == 0) {
        void* stack_addr = nullptr;
        size_t stack_size = 0;
        pthread_attr_getstack(&attr, &stack_addr, &stack_size);
        pthread_attr_destroy(&attr);
        return reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(stack_addr) + stack_size);
    }
    return nullptr;
}
#else
[[maybe_unused]] static void* DetectThreadStackBase() {
    return nullptr;
}
#endif

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
    if (nursery_size > 0) {
        young_budget_ = nursery_size;
        min_young_budget_ = std::min<size_t>(kMinYoungBudget, nursery_size);
        max_young_budget_ = std::max<size_t>(min_young_budget_, nursery_size * 4);
    } else {
        young_budget_ = kDefaultYoungBudget;
        min_young_budget_ = kMinYoungBudget;
        max_young_budget_ = kMaxYoungBudget;
    }
    old_gen_.SetBudget(budget_);
    if (nursery_.HasReservation()) {
        old_gen_.SetReservation(nursery_.OldGenStart(), kHeapReservationSize - (nursery_size * 2));
    }
    concurrent_marker_ = std::make_unique<ConcurrentMarker>(*this);
    if (Options::Get().max_heap_mb > 0) {
        budget_->SetTotalLimit(Options::Get().max_heap_mb * 1024 * 1024);
    }
    stack_base_ = nullptr;
    previous_current_heap_ = s_current_heap_;
    s_current_heap_ = this;
}

IsolateHeap::~IsolateHeap() {
    if (concurrent_marker_) {
        concurrent_marker_->Stop();
    }
    collecting_ = true;
    young_objects_.clear();
    remembered_set_.clear();
    weak_containers_.clear();
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
    if (auto* interp = Interpreter::Current()) {
        interp->IterateRoots(visitor);
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
    if (auto* interp = Interpreter::Current()) {
        LambdaSlotVisitor slot_visitor(visitor);
        interp->IterateRoots(&slot_visitor);
    }
}

std::vector<HeapObject*> IsolateHeap::Trace(const std::vector<HeapObject*>& roots) {
    auto mark_start = std::chrono::steady_clock::now();
    old_gen_.ClearMarks();
    for (auto* obj : young_objects_) {
        obj->SetMarked(false);
        obj->SetPinned(false);
    }

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
                    old_gen_.MarkObject(obj);
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
            old_gen_.MarkObject(root);
            mark_stack_.push_back(root);
            live.push_back(root);
            if (root->IsWeakMap()) {
                weak_maps.push_back(static_cast<JSWeakMap*>(root));
            }
        }
    }

    // 2. Conservative native stack scan (D2)
    ScanStack([&](HeapObject* obj) {
        if (!obj) return;
        obj->SetPinned(true);
        if (!obj->IsMarked()) {
            obj->SetMarked(true);
            old_gen_.MarkObject(obj);
            mark_stack_.push_back(obj);
            live.push_back(obj);
            if (obj->IsWeakMap()) {
                weak_maps.push_back(static_cast<JSWeakMap*>(obj));
            }
        }
    });

    // 3. Drain mark stack (DFS order)
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
    auto gc_start = std::chrono::steady_clock::now();

    class MinorMarkVisitor final : public SlotVisitor {
    public:
        explicit MinorMarkVisitor(std::vector<HeapObject*>& mark_stack)
            : mark_stack_(mark_stack) {}

        void Visit(TaggedValue* slot) override {
            if (!slot) return;
            TaggedValue val = *slot;
            if (val.IsHeapObject()) {
                HeapObject* obj = val.AsHeapObject();
                if (obj && obj->IsYoung() && !obj->IsMarked()) {
                    obj->SetMarked(true);
                    mark_stack_.push_back(obj);
                }
            }
        }
    private:
        std::vector<HeapObject*>& mark_stack_;
    };

    MinorMarkVisitor visitor(mark_stack_);

    // 1. Visit roots
    VisitRoots(&visitor);

    // 2. Visit native stack conservatively (pinned roots)
    ScanStack([&](HeapObject* obj) {
        if (!obj) return;
        obj->SetPinned(true);
        if (obj->IsYoung() && !obj->IsMarked()) {
            obj->SetMarked(true);
            mark_stack_.push_back(obj);
        }
    });

    // 3. Mark from Remembered Set
    for (HeapObject* old_source : remembered_set_) {
        if (old_source && old_source->IsOld()) {
            old_source->IteratePointers(&visitor);
        }
    }

    if (ConcurrentMarker::Current() && ConcurrentMarker::Current()->IsActive()) {
        for (HeapObject* old_obj : old_gen_.Objects()) {
            if (old_obj && old_obj->IsOld()) {
                old_obj->IteratePointers(&visitor);
            }
        }
    }

    // 4. Drain mark stack
    while (!mark_stack_.empty()) {
        HeapObject* obj = mark_stack_.back();
        mark_stack_.pop_back();
        obj->IteratePointers(&visitor);
    }

    // 5. Ephemeron fixpoint for weak maps
    while (true) {
        size_t before = mark_stack_.size();
        for (HeapObject* obj : weak_containers_) {
            if (obj && obj->IsWeakMap() && (obj->IsOld() || obj->IsMarked())) {
                static_cast<JSWeakMap*>(obj)->TraceEphemerons(
                    [](HeapObject* key) { return key && (key->IsOld() || key->IsMarked()); },
                    &visitor);
            }
        }
        while (!mark_stack_.empty()) {
            HeapObject* obj = mark_stack_.back();
            mark_stack_.pop_back();
            obj->IteratePointers(&visitor);
        }
        if (mark_stack_.size() == before) break;
    }

    // 6. Process weak references
    std::vector<HeapObject*> live_weak_containers;
    live_weak_containers.reserve(weak_containers_.size());
    for (HeapObject* obj : weak_containers_) {
        if (!obj) continue;
        bool is_container_alive = obj->IsOld() || obj->IsMarked();
        if (!is_container_alive) {
            continue;
        }
        live_weak_containers.push_back(obj);
        if (obj->IsWeakRef()) {
            auto* weak = static_cast<JSWeakRef*>(obj);
            if (weak->Target() && weak->Target()->IsYoung() && !weak->Target()->IsMarked()) {
                weak->SetTarget(nullptr);
            }
        } else if (obj->IsWeakMap()) {
            static_cast<JSWeakMap*>(obj)->PruneDeadKeys([](HeapObject* key) {
                return key && (key->IsOld() || key->IsMarked());
            });
        } else if (obj->IsWeakSet()) {
            static_cast<JSWeakSet*>(obj)->PruneDeadEntries([](HeapObject* val) {
                return val && (val->IsOld() || val->IsMarked());
            });
        } else if (obj->IsFinalizationRegistry()) {
            static_cast<JSFinalizationRegistry*>(obj)->ProcessWeakTargets(
                [](HeapObject* target) -> HeapObject* {
                    if (!target) return nullptr;
                    if (target->IsOld() || target->IsMarked()) return target;
                    return nullptr;
                },
                [this](TaggedValue callback, TaggedValue held) {
                    finalizers_.push_back({callback, held});
                });
        }
    }
    weak_containers_.swap(live_weak_containers);

    // 7. Sweep and promote in Immix
    size_t allocated_before = young_bytes_allocated_;
    size_t surviving_bytes = 0;
    std::vector<HeapObject*> surviving_young;
    surviving_young.reserve(young_objects_.size() / 2);
    std::vector<HeapObject*> newly_tenured;

    for (HeapObject* obj : young_objects_) {
        if (!obj) continue;
        if (obj->IsMarked()) {
            // Survivor!
            size_t bytes = obj->GetAllocatedSize();
            if (bytes == 0) bytes = sizeof(HeapObject);
            surviving_bytes += bytes;
            obj->IncrementAge();
            obj->SetMarked(false);
            obj->SetPinned(false);
            if (obj->GetAge() >= kTenuringAgeThreshold) {
                obj->SetOld(true);
                newly_tenured.push_back(obj);
                old_bytes_allocated_ += bytes;
            } else {
                surviving_young.push_back(obj);
            }

            if (ImmixBlock* block = old_gen_.BlockForPointer(obj)) {
                size_t start_line = block->LineFor(obj);
                size_t end_line = block->LineFor(reinterpret_cast<const uint8_t*>(obj) + bytes - 1);
                for (size_t l = start_line; l <= end_line && l < kLinesPerBlock; ++l) {
                    block->line_marks[l] = 1;
                }
            }
        } else {
            // Dead young object: reclaim in-place
            size_t bytes = obj->GetAllocatedSize();
            bool was_charged = obj->IsCharged();
            obj->~HeapObject();
            if (ImmixBlock* block = old_gen_.BlockForPointer(obj)) {
                block->ClearObjectStart(obj);
                if (Options::Get().gc_verify) {
                    std::memset(static_cast<void*>(obj), kNurseryPoisonByte, bytes);
                }
            } else if (old_gen_.IsLargeObject(obj)) {
                old_gen_.FreeLarge(LargeObjectHeader::FromObject(obj));
            }
            if (was_charged && budget_) budget_->ReleaseManaged(bytes);
        }
    }
    young_objects_.swap(surviving_young);
    young_bytes_allocated_ = 0;

    // Adaptive young budget adjustment based on survival rate
    if (allocated_before > 0) {
        double survival_rate = static_cast<double>(surviving_bytes) / allocated_before;
        if (survival_rate > 0.6) {
            young_budget_ = std::min(max_young_budget_, static_cast<size_t>(young_budget_ * 1.5));
        } else if (survival_rate < 0.2) {
            young_budget_ = std::max(min_young_budget_, static_cast<size_t>(young_budget_ * 0.8));
        }
    }

    const bool budget_pressure = (HeapBudget() > 0 && TotalAllocatedBytes() >= HeapBudget() * 75 / 100);
    if (old_bytes_allocated_ >= old_gen_growth_threshold_ || budget_pressure) {
        RequestSafepoint();
    }

    // Check newly tenured objects: if they reference young objects, add to remembered set
    for (HeapObject* obj : newly_tenured) {
        bool points_to_young = false;
        LambdaSlotVisitor check_young([&](TaggedValue* s) {
            if (s && s->IsHeapObject() && s->AsHeapObject()->IsYoung()) {
                points_to_young = true;
            }
        });
        obj->IteratePointers(&check_young);
        if (points_to_young && !obj->IsRemembered()) {
            obj->SetRemembered(true);
            remembered_set_.push_back(obj);
            card_table_.MarkCard(obj);
        }
    }

    // 8. Prune remembered set: retain only old objects that still point to young objects
    std::vector<HeapObject*> active_remembered;
    for (HeapObject* old_source : remembered_set_) {
        if (!old_source || !old_source->IsOld()) continue;
        bool still_points_to_young = false;
        LambdaSlotVisitor check_visitor([&](TaggedValue* slot) {
            if (slot && slot->IsHeapObject() && slot->AsHeapObject()->IsYoung()) {
                still_points_to_young = true;
            }
        });
        old_source->IteratePointers(&check_visitor);
        if (still_points_to_young) {
            active_remembered.push_back(old_source);
        } else {
            old_source->SetRemembered(false);
            card_table_.ClearCard(old_source);
        }
    }
    remembered_set_.swap(active_remembered);

    ++minor_gc_count_;

    if (Options::Get().gc_verify_barrier || Options::Get().gc_verify) {
        VerifyBarrierInvariant();
    }

    HeapVerifier::VerifyAfterGC(*this);

    if (Options::Get().trace_gc) {
        auto gc_end = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(gc_end - gc_start).count();
        std::cout << "[GC] MinorGC #" << minor_gc_count_ << " completed in " << ms
                  << "ms: live young=" << young_objects_.size() << ", remembered=" << remembered_set_.size() << std::endl;
    }
}

void IsolateHeap::VerifyBarrierInvariant() {
    for (HeapObject* old_obj : old_gen_.Objects()) {
        if (!old_obj || !old_obj->IsOld()) continue;
        LambdaSlotVisitor check_visitor([&](TaggedValue* slot) {
            if (slot && slot->IsHeapObject()) {
                HeapObject* child = slot->AsHeapObject();
                if (child && child->IsYoung()) {
                    if (!old_obj->IsRemembered()) {
                        throw std::runtime_error(
                            "Barrier Invariant Violation: Old object at " +
                            std::to_string(reinterpret_cast<uintptr_t>(old_obj)) +
                            " points to young object at " +
                            std::to_string(reinterpret_cast<uintptr_t>(child)) +
                            " but is not in the remembered set!");
                    }
                }
            }
        });
        old_obj->IteratePointers(&check_visitor);
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

    // In Major GC, any live young object promotes to old
    for (auto* obj : young_objects_) {
        if (obj && obj->IsMarked()) {
            obj->SetOld(true);
        }
    }
    young_objects_.clear();
    young_bytes_allocated_ = 0;

    std::vector<HeapObject*> live_weak_containers;
    for (HeapObject* obj : weak_containers_) {
        if (obj && obj->IsMarked()) {
            live_weak_containers.push_back(obj);
        }
    }
    weak_containers_.swap(live_weak_containers);

    // Clear remembered set since all live objects are now Old
    for (auto* obj : remembered_set_) {
        if (obj) obj->SetRemembered(false);
    }
    remembered_set_.clear();

    old_gen_.Sweep();
    card_table_.Clear();
    ++major_gc_count_;

    size_t post_major_live_bytes = 0;
    for (auto* obj : live) {
        if (obj) post_major_live_bytes += obj->GetAllocatedSize();
    }
    last_major_live_bytes_ = post_major_live_bytes;
    old_bytes_allocated_ = 0;
    safepoint_requested_ = false;

    // Major GC threshold: 1.5-2x over post-last-major live size (min 32 MB)
    size_t target_threshold = std::max(kMinMajorGCThreshold, static_cast<size_t>(last_major_live_bytes_ * 1.75));
    if (budget_->TotalLimit() > 0 && target_threshold > budget_->TotalLimit()) {
        target_threshold = std::max<size_t>(1024, budget_->TotalLimit() * 75 / 100);
    }
    old_gen_growth_threshold_ = target_threshold;

    HeapVerifier::VerifyAfterGC(*this);
    if (Options::Get().trace_gc) {
        std::cout << "[GC] MajorGC #" << major_gc_count_ << ": live=" << live.size() << " objects, usage=" << (budget_->ChargedBytes() / 1024) << "KB" << std::endl;
    }
}

bool IsolateHeap::CollectAtSafepoint() {
    AssertOwnerThread();
    if (!GCAllowed()) return false;
    safepoint_requested_ = false;
    const bool budget_pressure = (HeapBudget() > 0 && TotalAllocatedBytes() >= HeapBudget() * 75 / 100);
    if (budget_pressure || old_bytes_allocated_ >= old_gen_growth_threshold_ || young_objects_.empty()) {
        MajorGC();
    } else {
        MinorGC();
        if ((budget_->TotalLimit() > 0 && (minor_gc_count_ % 10) == 0) || (minor_gc_count_ % 50) == 0) {
            MajorGC();
        }
    }
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

bool IsolateHeap::IsValidObjectStart(const void* ptr) const noexcept {
    return old_gen_.IsValidObjectStart(ptr);
}

#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable: 4611)
#endif
AETHER_NOINLINE
static void CaptureRegisters(jmp_buf env) {
    setjmp(env);
}
#if defined(_MSC_VER)
#pragma warning(pop)
#endif

AETHER_NOINLINE
AETHER_NO_SANITIZE_ADDRESS
void IsolateHeap::ScanStack(const std::function<void(HeapObject*)>& on_root) {
    AssertOwnerThread();
    if (!stack_base_) return;
    void* base_ptr = stack_base_;

    jmp_buf jmp_buffer;
    CaptureRegisters(jmp_buffer);

#if defined(_WIN32) && defined(_M_X64)
    CONTEXT win_context;
    RtlCaptureContext(&win_context);
#endif

    uintptr_t sp = reinterpret_cast<uintptr_t>(GetCurrentStackPointer()) & ~(sizeof(void*) - 1);
    uintptr_t base = reinterpret_cast<uintptr_t>(base_ptr);

    // Stacks grow downward on all supported targets (x86-64, ARM64):
    // stack_base_ (caller) must be greater than current sp (callee).
    if (sp >= base) {
        return;
    }

    constexpr size_t kMaxScanWords = (8 * 1024 * 1024) / sizeof(uintptr_t);
    if ((base - sp) / sizeof(uintptr_t) > kMaxScanWords) {
        return;
    }

#if defined(AETHER_HAS_ASAN_INTERFACE)
    void* fake_stack = __asan_get_current_fake_stack();
#endif

    auto* current = reinterpret_cast<uintptr_t*>(sp);
    auto* limit   = reinterpret_cast<uintptr_t*>(base);

    while (current < limit) {
        uintptr_t word = *current++;

        // 1. Check if word is a raw pointer to a valid HeapObject start
        if (word != 0 && (word & (kObjectAlignment - 1)) == 0) {
            void* raw_ptr = reinterpret_cast<void*>(word);
            if (IsValidObjectStart(raw_ptr)) {
                on_root(static_cast<HeapObject*>(raw_ptr));
                continue;
            }
        }

        // 2. Check if word is a TaggedValue holding a HeapObject
        if ((word & TaggedValue::kTagMask) == TaggedValue::kTagHeapObject) {
            uintptr_t payload = word & TaggedValue::kPayloadMask;
            if (payload != 0 && (payload & (kObjectAlignment - 1)) == 0) {
                void* tagged_ptr = reinterpret_cast<void*>(payload);
                if (IsValidObjectStart(tagged_ptr)) {
                    on_root(static_cast<HeapObject*>(tagged_ptr));
                }
            }
        }

#if defined(AETHER_HAS_ASAN_INTERFACE)
        if (fake_stack != nullptr && word != 0) {
            void* beg = nullptr;
            void* end = nullptr;
            void* real_frame = __asan_addr_is_in_fake_stack(fake_stack, reinterpret_cast<void*>(word), &beg, &end);
            if (real_frame != nullptr && beg != nullptr && end != nullptr && beg < end) {
                auto* f_cur = reinterpret_cast<uintptr_t*>(beg);
                auto* f_lim = reinterpret_cast<uintptr_t*>(end);
                while (f_cur < f_lim) {
                    uintptr_t f_word = *f_cur++;
                    if (f_word != 0 && (f_word & (kObjectAlignment - 1)) == 0) {
                        void* r_ptr = reinterpret_cast<void*>(f_word);
                        if (IsValidObjectStart(r_ptr)) {
                            on_root(static_cast<HeapObject*>(r_ptr));
                            continue;
                        }
                    }
                    if ((f_word & TaggedValue::kTagMask) == TaggedValue::kTagHeapObject) {
                        uintptr_t f_payload = f_word & TaggedValue::kPayloadMask;
                        if (f_payload != 0 && (f_payload & (kObjectAlignment - 1)) == 0) {
                            void* t_ptr = reinterpret_cast<void*>(f_payload);
                            if (IsValidObjectStart(t_ptr)) {
                                on_root(static_cast<HeapObject*>(t_ptr));
                            }
                        }
                    }
                }
            }
        }
#endif
    }
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
