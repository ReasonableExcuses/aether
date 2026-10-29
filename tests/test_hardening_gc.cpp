#include "aether/heap.h"
#include "aether/advanced_intrinsics.h"
#include "aether/concurrent_marker.h"
#include "aether/memory_budget.h"

#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

using namespace aether;

namespace {
void Check(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

template <typename F>
void BudgetFailure(F&& fn) {
    bool failed = false;
    try { fn(); } catch (const std::length_error&) { failed = true; }
    Check(failed, "expected budget rejection");
}

struct Resource {
    explicit Resource(int& destroyed) : destroyed(destroyed) {}
    ~Resource() { ++destroyed; }
    int& destroyed;
};

// ObjectSize intentionally includes a fictitious external extent. Collection
// must use the allocation record, never this legacy diagnostic estimate.
class Probe : public HeapObject {
public:
    explicit Probe(int& destroyed, size_t external = 0)
        : HeapObject(HeapObjectType::FixedArray, nullptr), charge_(external),
          resource_(std::make_unique<Resource>(destroyed)) {}
    Probe(Probe&&) noexcept = default;
    HeapObject* RelocateTo(void* dst) override { return new (dst) Probe(std::move(*this)); }
    size_t ObjectSize() const override { return sizeof(Probe) + 1024 * 1024; }
    void IteratePointers(SlotVisitor* visitor) override {
        Check(owner == std::this_thread::get_id(), "off-thread graph traversal");
        visitor->Visit(&child);
    }
    TaggedValue child{TaggedValue::Undefined()};
    const std::thread::id owner{std::this_thread::get_id()};
private:
    MemoryCharge charge_;
    std::unique_ptr<Resource> resource_;
};

class ConstructorProbe : public HeapObject {
public:
    explicit ConstructorProbe(IsolateHeap& heap)
        : HeapObject(HeapObjectType::FixedArray, nullptr) {
        Check(!heap.GCAllowed(), "constructor must inhibit collection");
        child = TaggedValue::FromHeapObject(heap.Allocate<JSString>("nested"));
        Check(!heap.CollectAtSafepoint(), "constructor safepoint must be inhibited");
        heap.MinorGC();
        heap.MajorGC();
    }
    ConstructorProbe(ConstructorProbe&&) noexcept = default;
    HeapObject* RelocateTo(void* dst) override { return new (dst) ConstructorProbe(std::move(*this)); }
    void IteratePointers(SlotVisitor* visitor) override { visitor->Visit(&child); }
    TaggedValue child;
};

class ThrowingProbe : public HeapObject {
public:
    ThrowingProbe() : HeapObject(HeapObjectType::FixedArray, nullptr), charge_(123) {
        throw std::runtime_error("constructor failure");
    }
    HeapObject* RelocateTo(void*) override { std::terminate(); }
private:
    MemoryCharge charge_;
};

class LargeProbe : public Probe {
public:
    explicit LargeProbe(int& destroyed) : Probe(destroyed) {}
    LargeProbe(LargeProbe&&) noexcept = default;
    HeapObject* RelocateTo(void* dst) override { return new (dst) LargeProbe(std::move(*this)); }
    char payload[kImmixBlockSize + 8]{};
};

void SurvivorsAndHandles() {
    IsolateHeap heap(16 * 1024);
    HandleScope outer(heap);
    auto array = outer.CreateHandle(heap.Allocate<JSArray>(nullptr, 100));
    array->SetElement(99, TaggedValue::FromSmi(73));
    TaggedValue* slot = array.Slot();
    for (int i = 0; i < 10000; ++i) outer.CreateHandle(TaggedValue::FromSmi(i));
    Check(slot == array.Slot() && slot->AsHeapObject() == array.Get(), "handle slots must be stable");
    {
        HandleScope inner(heap);
        auto child = inner.CreateHandle(heap.Allocate<JSString>("inner"));
        heap.MinorGC();
        Check(child->Value() == "inner", "nested inner root lost");
        Check(array->GetElement(99).AsSmi() == 73, "outer root lost");
    }
    JSArray* survivor = array.Get();
    auto* next = heap.Allocate<JSObject>(nullptr);
    Check(reinterpret_cast<uintptr_t>(next) >= reinterpret_cast<uintptr_t>(survivor) + sizeof(JSArray),
          "allocation overwrote evacuated survivor");
    Check(array->GetElement(99).AsSmi() == 73, "survivor damaged after allocation");
    array->SetElement(0, TaggedValue::FromHeapObject(heap.Allocate<JSArray>(nullptr, 4)));
    heap.MinorGC(); // array promotes; newborn child must still be traced
    Check(array->IsOld(), "parent did not promote");
    auto* young_child = array->GetElement(0).As<JSArray>();
    Check(heap.GetNursery().Contains(young_child) && young_child->Length() == 4, "promoted child not repaired");
    heap.MinorGC();
    Check(array->GetElement(0).As<JSArray>()->IsOld(), "child did not survive next cycle");
    for (int i = 0; i < 10000; ++i) heap.Allocate<JSObject>(nullptr);
    Check(array->GetElement(99).AsSmi() == 73 && heap.MinorGCCount() > 5, "allocation stress lost root");
}

void LifetimesAndConstructors() {
    int destroyed = 0;
    std::shared_ptr<MemoryBudgetState> state;
    {
        IsolateHeap heap(16 * 1024);
        state = heap.BudgetState();
        heap.Allocate<Probe>(destroyed, 200);
        heap.MinorGC();
        Check(destroyed == 1 && heap.TotalAllocatedBytes() == 0, "dead nursery resource leaked");
        {
            HandleScope roots(heap);
            auto live = roots.CreateHandle(heap.Allocate<Probe>(destroyed, 300));
            const size_t charged = heap.TotalAllocatedBytes();
            heap.MinorGC();
            heap.MinorGC();
            Check(live->IsOld() && destroyed == 1, "move destroyed surviving resource");
            Check(heap.TotalAllocatedBytes() == charged, "promotion double charged or lost charge");
            auto nested = roots.CreateHandle(heap.Allocate<ConstructorProbe>(heap));
            Check(nested->child.As<JSString>()->Value() == "nested", "nested constructor allocation lost");
        }
        heap.MajorGC();
        Check(destroyed == 2 && heap.TotalAllocatedBytes() == 0, "old resource or nested constructor leaked");
        {
            HandleScope roots(heap);
            auto large = roots.CreateHandle(heap.AllocateOld<LargeProbe>(destroyed));
            large->payload[kImmixBlockSize] = 42;
            heap.MajorGC();
            Check(large->payload[kImmixBlockSize] == 42, "large old object lost");
        }
        heap.MajorGC();
        Check(destroyed == 3 && heap.TotalAllocatedBytes() == 0, "large object lifetime leaked");
        for (bool old : {false, true}) {
            try {
                if (old) heap.AllocateOld<ThrowingProbe>();
                else heap.Allocate<ThrowingProbe>();
                Check(false, "constructor did not throw");
            } catch (const std::runtime_error&) {}
            Check(heap.TotalAllocatedBytes() == 0, "failed construction leaked charge");
        }
        heap.Allocate<Probe>(destroyed, 20);
        heap.AllocateOld<Probe>(destroyed, 30);
        AllocateRuntime<Probe>(destroyed, 40);
    }
    Check(destroyed == 6 && state->ChargedBytes() == 0, "shutdown did not destroy all objects");
}

void BudgetsAndRuntimePolicy() {
    IsolateHeap heap;
    Check(heap.HeapBudget() == 512ULL * 1024 * 1024, "incorrect default total budget");
    heap.SetHeapBudget(1000);
    {
        MemoryCharge a(600);
        MemoryCharge b(std::move(a));
        Check(a.Bytes() == 0 && b.Bytes() == 600, "move did not transfer charge");
        MemoryCharge c(100);
        c = std::move(b);
        Check(heap.TotalAllocatedBytes() == 600 && b.Bytes() == 0, "move assignment leaked old charge");
        const size_t cycles = heap.MajorGCCount();
        BudgetFailure([&] { c.Resize(1001); });
        Check(c.Bytes() == 600 && heap.TotalAllocatedBytes() == 600, "failed Resize modified accounting");
        Check(heap.MajorGCCount() == cycles, "Resize ran GC");
        c.Resize(200);
        Check(heap.TotalAllocatedBytes() == 200, "shrink failed to release charge");
    }
    Check(heap.TotalAllocatedBytes() == 0, "charge destructor leaked");
    heap.SetHeapBudget(0); // unlimited total; aggregate ArrayBuffer cap remains
    {
        MemoryCharge buffer(2ULL * 1024 * 1024 * 1024 - 10, true);
        MemoryCharge remaining(10, true);
        BudgetFailure([&] { MemoryCharge overflow(1, true); });
        BudgetFailure([&] { buffer.Resize(std::numeric_limits<size_t>::max()); });
        Check(heap.BudgetState()->ArrayBufferBytes() == 2ULL * 1024 * 1024 * 1024, "aggregate ArrayBuffer accounting");
    }
    int destroyed = 0;
    constexpr size_t bytes = (sizeof(Probe) + kObjectAlignment - 1) & ~(kObjectAlignment - 1);
    heap.SetHeapBudget(bytes * 2);
    {
        HandleScope roots(heap);
        auto live = roots.CreateHandle(heap.AllocateOld<Probe>(destroyed));
        heap.AllocateOld<Probe>(destroyed);
        heap.AllocateOld<Probe>(destroyed); // collect/recheck, reclaim garbage
        Check(heap.MajorGCCount() > 0 && destroyed == 1, "managed pressure must collect and recheck");
        NoGCScope no_gc(heap);
        BudgetFailure([&] { AllocateRuntime<Probe>(destroyed); });
        Check(!heap.CollectAtSafepoint() && live.Get(), "NoGCScope violated");
    }
    heap.MajorGC();
    Check(destroyed == 3 && heap.TotalAllocatedBytes() == 0, "managed accounting is cumulative");
    heap.SetHeapBudget(kDefaultHeapBudget);
    {
        HandleScope roots(heap);
        auto stable = roots.CreateHandle(AllocateRuntime<Probe>(destroyed));
        auto* address = stable.Get();
        heap.MinorGC();
        heap.MajorGC();
        Check(stable.Get() == address, "runtime object moved");
    }
    heap.CollectAtSafepoint();
    Check(destroyed == 4, "runtime objects were silently rooted forever");
    {
        IsolateScope none(nullptr);
        AllocateRuntime<Probe>(destroyed);
        Check(IsolateHeap::Current() == nullptr, "fallback changed active isolate");
        IsolateHeap::RuntimeFallbackHeap().CollectAtSafepoint();
        Check(destroyed == 5, "fallback allocation is unowned");
    }
    std::unique_ptr<MemoryCharge> survivor;
    std::shared_ptr<MemoryBudgetState> state;
    {
        IsolateHeap temporary;
        state = temporary.BudgetState();
        survivor = std::make_unique<MemoryCharge>(400);
    }
    Check(state->ChargedBytes() == 400, "external owner charge lost at teardown");
    survivor.reset();
    Check(state->ChargedBytes() == 0, "late charge release failed");
    IsolateHeap::SetCurrent(&heap);
}

void WeakRelocationAndEphemerons() {
    IsolateHeap heap;
    HandleScope roots(heap);
    auto key = roots.CreateHandle(heap.Allocate<JSObject>(nullptr));
    auto map1 = roots.CreateHandle(heap.Allocate<JSWeakMap>());
    auto map2 = roots.CreateHandle(heap.Allocate<JSWeakMap>());
    auto set = roots.CreateHandle(heap.Allocate<JSWeakSet>());
    auto ref = roots.CreateHandle(heap.Allocate<JSWeakRef>(key.Get()));
    auto* key2 = heap.Allocate<JSObject>(nullptr);
    auto* value = heap.Allocate<JSString>("ephemeron-chain");
    map1->Set(key.Get(), TaggedValue::FromHeapObject(key2));
    map2->Set(key2, TaggedValue::FromHeapObject(value));
    set->Add(key.Get());
    heap.MinorGC();
    Check(ref->Target() == key.Get() && set->Has(key.Get()), "weak target/key not relocated");
    auto moved_key2 = map1->Get(key.Get());
    Check(map2->Get(moved_key2.AsHeapObject()).As<JSString>()->Value() == "ephemeron-chain", "minor ephemeron chain lost");
    heap.MinorGC();
    heap.MajorGC();
    Check(map2->Get(map1->Get(key.Get()).AsHeapObject()).As<JSString>()->Value() == "ephemeron-chain", "major fixed point incomplete");
    *key.Slot() = TaggedValue::Undefined();
    heap.MajorGC();
    Check(ref->Deref().IsUndefined() && map1->Size() == 0 && map2->Size() == 0 && set->Size() == 0,
          "major weak targets must clear before poison");
    auto* cyclic_key = heap.Allocate<JSObject>(nullptr);
    map1->Set(cyclic_key, TaggedValue::FromHeapObject(cyclic_key));
    heap.MinorGC();
    Check(map1->Size() == 0, "ephemeron self-cycle incorrectly kept its key alive");
    auto* dead = heap.Allocate<JSObject>(nullptr);
    ref->SetTarget(dead);
    heap.MinorGC();
    Check(ref->Deref().IsUndefined(), "minor weak target not cleared");

    // Weak container nodes/buckets consume the same external budget. Moves at
    // an exact budget must not allocate new sentinels or duplicate charges.
    auto fresh_map = roots.CreateHandle(heap.Allocate<JSWeakMap>());
    auto fresh_key = roots.CreateHandle(heap.Allocate<JSObject>(nullptr));
    const size_t before = heap.TotalAllocatedBytes();
    heap.SetHeapBudget(before);
    BudgetFailure([&] { fresh_map->Set(fresh_key.Get(), TaggedValue::True()); });
    Check(fresh_map->Size() == 0 && heap.TotalAllocatedBytes() == before, "weak insertion failure not atomic");
    heap.MinorGC();
    Check(heap.TotalAllocatedBytes() == before, "weak container relocation allocated or lost charge");
    heap.SetHeapBudget(kDefaultHeapBudget);
}

void FinalizationQueue() {
    IsolateHeap heap;
    HandleScope roots(heap);
    auto callback = roots.CreateHandle(heap.Allocate<JSString>("callback-token"));
    auto registry = roots.CreateHandle(heap.Allocate<JSFinalizationRegistry>(callback.GetTagged()));
    auto target = roots.CreateHandle(heap.Allocate<JSObject>(nullptr));
    auto token = roots.CreateHandle(heap.Allocate<JSObject>(nullptr));
    registry->Register(target.Get(), TaggedValue::FromSmi(1), token.Get());
    heap.MinorGC();
    Check(registry->Unregister(token.Get()), "unregister token not relocated");
    auto* held = heap.Allocate<JSString>("held-resource");
    registry->Register(target.Get(), TaggedValue::FromHeapObject(held));
    *target.Slot() = TaggedValue::Undefined();
    *callback.Slot() = TaggedValue::Undefined();
    heap.MinorGC();
    Check(heap.PendingFinalizationCount() == 1 && registry->RegistrationCount() == 0, "cleanup not queued exactly once");
    *registry.Slot() = TaggedValue::Undefined();
    heap.MinorGC();
    heap.MajorGC(); // queue alone must retain callback and held value
    int invoked = 0;
    heap.DrainFinalizationQueue([&](TaggedValue cb, TaggedValue value) {
        ++invoked;
        Check(cb.As<JSString>()->Value() == "callback-token", "queued callback unrooted");
        Check(value.As<JSString>()->Value() == "held-resource", "queued held value unrooted");
        Check(heap.DrainFinalizationQueue([](TaggedValue, TaggedValue) {}) == 0, "reentrant drain");
    });
    Check(invoked == 1 && heap.PendingFinalizationCount() == 0, "cleanup queue drain failed");
    *token.Slot() = TaggedValue::Undefined();
    heap.MajorGC();
    Check(heap.TotalAllocatedBytes() == 0, "drained cleanup resources retained");
}

void ConsecutiveMarkerCycles() {
    IsolateHeap heap(16 * 1024);
    HandleScope roots(heap);
    int destroyed = 0;
    auto root = roots.CreateHandle(heap.AllocateOld<Probe>(destroyed));
    Check(!ConcurrentMarker::SupportsConcurrentTraversal(), "unsafe background traversal enabled");
    for (int cycle = 0; cycle < 6; ++cycle) {
        heap.StartConcurrentMark();
        for (int i = 0; i < 100; ++i) {
            // No barrier on this test field: finalization must retrace.
            root->child = TaggedValue::FromHeapObject(heap.Allocate<Probe>(destroyed));
        }
        heap.MinorGC();
        heap.FinalizeConcurrentGC();
        Check(root->IsMarked() && root->child.AsHeapObject()->IsMarked(), "consecutive marker cycle lost graph");
        Check(!heap.GetConcurrentMarker()->IsActive(), "marker stuck active");
    }
    {
        ConcurrentMarker marker(heap);
        auto* explicit_root = heap.Allocate<Probe>(destroyed);
        marker.Start({explicit_root});
        heap.MinorGC(); // explicit marker root slots must relocate too
        marker.WaitForCompletion();
        marker.FinalizeMarking();
        Check(marker.MarkedObjectCount() >= 3, "marker explicit root lost");
        marker.Stop();
    }
    {
        // Low-level placement-new clients opt in to ownership explicitly.
        void* memory = heap.GetOldGen().Allocate(sizeof(Probe));
        auto* raw = new (memory) Probe(destroyed);
        raw->SetOld(true);
        heap.GetOldGen().RegisterObject(raw);
        auto weak = roots.CreateHandle(heap.AllocateOld<JSWeakRef>(raw));
        ConcurrentMarker marker(heap);
        marker.Start({weak.Get()});
        marker.FinalizeMarking();
        heap.GetOldGen().Sweep();
        Check(weak->Deref().IsUndefined(), "standalone marker swept before clearing weak target");
        marker.Stop();
        *weak.Slot() = TaggedValue::Undefined();
    }
    std::atomic<bool> rejected{false};
    std::thread foreign([&] {
        try { heap.MajorGC(); } catch (const std::logic_error&) { rejected = true; }
    });
    foreign.join();
    Check(rejected, "off-thread collection was permitted");
    *root.Slot() = TaggedValue::Undefined();
    heap.MajorGC();
    Check(destroyed == 603 && heap.TotalAllocatedBytes() == 0, "marker retained dead graph or skipped destruction");
}
} // namespace

int main() {
    try {
        SurvivorsAndHandles();
        LifetimesAndConstructors();
        BudgetsAndRuntimePolicy();
        WeakRelocationAndEphemerons();
        FinalizationQueue();
        ConsecutiveMarkerCycles();
        std::cout << "GC/allocator hardening: 6 regression groups passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "GC hardening failure: " << ex.what() << '\n';
        return 1;
    }
}
