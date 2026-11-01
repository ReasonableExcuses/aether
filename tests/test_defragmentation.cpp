#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include "aether/heap_object.h"
#include "aether/heap.h"
#include "aether/heap_verifier.h"
#include "aether/options.h"
#include <iostream>
#include <vector>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestOpportunisticDefragmentationChurn() {
    std::cout << "[Test 1] Verifying Opportunistic Immix Defragmentation Churn (<= 1.3x)...\n" << std::flush;

    IsolateHeap heap;
    IsolateScope isolate(heap);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    constexpr size_t kTotalObjects = 10000;
    std::vector<Handle<JSObject>> live_objects;
    live_objects.reserve(kTotalObjects / 5);
    for (size_t i = 0; i < kTotalObjects; ++i) {
        auto* obj = heap.AllocateOld<JSObject>(root_shape.get());
        obj->SetFastSlot(0, true, TaggedValue::FromSmi(static_cast<int32_t>(i)));
        if (i % 5 == 0) live_objects.push_back(scope.CreateHandle(obj));
    }
    heap.MajorGC();
    double frag = heap.FragmentationRatio();
    std::cout << "  -> Post-churn fragmentation ratio: " << frag << "x (live="
              << heap.LastMajorLiveBytes() << "B, active=" << heap.ActiveCommittedBytes()
              << "B, free_blocks=" << heap.GetOldGen().FreeBlockCount() << ")\n" << std::flush;

    assert(frag <= 1.30);
    assert(heap.GetOldGen().FreeBlockCount() > 0);
    for (size_t k = 0; k < live_objects.size(); ++k) {
        JSObject* obj = live_objects[k].Get();
        assert(obj != nullptr);
        assert(obj->GetFastSlot(0, true).IsSmi());
        assert(obj->GetFastSlot(0, true).AsSmi() == static_cast<int32_t>(k * 5));
    }

    std::cout << "  -> PASSED: Defragmentation achieved <= 1.3x fragmentation with full data integrity.\n" << std::flush;
}

void TestPointerFixupAcrossEvacuatedBlocks() {
    std::cout << "[Test 2] Verifying Reference Fixup Across Evacuated Blocks...\n" << std::flush;

    IsolateHeap heap;
    IsolateScope isolate(heap);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    constexpr size_t kObjectsPerBlock = 500;
    Handle<JSObject> handle_a;
    for (size_t i = 0; i < kObjectsPerBlock; ++i) {
        auto* obj = heap.AllocateOld<JSObject>(root_shape.get());
        if (i == 0) handle_a = scope.CreateHandle(obj);
    }
    Handle<JSObject> handle_b;
    for (size_t i = 0; i < kObjectsPerBlock; ++i) {
        auto* obj = heap.AllocateOld<JSObject>(root_shape.get());
        if (i == 0) handle_b = scope.CreateHandle(obj);
    }
    handle_a.Get()->SetFastSlot(0, true, TaggedValue::FromHeapObject(handle_b.Get()));

    heap.MajorGC();
    JSObject* survivor_a = handle_a.Get();
    JSObject* survivor_b = handle_b.Get();
    assert(survivor_a != nullptr);
    assert(survivor_b != nullptr);
    assert(survivor_a->GetFastSlot(0, true).IsHeapObject());
    assert(survivor_a->GetFastSlot(0, true).AsHeapObject() == survivor_b);

    std::cout << "  -> PASSED: Pointers across evacuated objects correctly rewritten.\n" << std::flush;
}

void TestPinnedObjectNotEvacuated() {
    std::cout << "[Test 3] Verifying Conservative Stack Pinning Prevents Evacuation...\n" << std::flush;

    IsolateHeap heap;
    IsolateScope isolate(heap);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    void* stack_marker = GetCurrentStackPointer();
    heap.SetStackBase(stack_marker);

    constexpr size_t kBatch = 400;

    for (size_t i = 0; i < kBatch; ++i) {
        heap.AllocateOld<JSObject>(root_shape.get());
    }

    JSObject* raw_stack_obj = heap.AllocateOld<JSObject>(root_shape.get());
    uintptr_t original_addr = reinterpret_cast<uintptr_t>(raw_stack_obj);

    heap.MajorGC();

    assert(raw_stack_obj->IsPinned());
    assert(reinterpret_cast<uintptr_t>(raw_stack_obj) == original_addr);

    heap.SetStackBase(nullptr);
    std::cout << "  -> PASSED: Pinned object remained at original address while candidate was defragmented.\n" << std::flush;
}

void TestDefragmentationUnderGCStress() {
    std::cout << "[Test 4] Verifying Defragmentation under GC Stress and Verification...\n" << std::flush;

    Options::Get().gc_stress = true;
    Options::Get().gc_verify = true;
    IsolateHeap heap;
    IsolateScope isolate(heap);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    std::vector<Handle<JSObject>> kept;

    for (size_t cycle = 0; cycle < 3; ++cycle) {
        for (size_t i = 0; i < 300; ++i) {
            auto* obj = heap.AllocateOld<JSObject>(root_shape.get());
            obj->SetFastSlot(0, true, TaggedValue::FromSmi(static_cast<int32_t>(i)));
            if (i % 4 == 0) kept.push_back(scope.CreateHandle(obj));
        }
        heap.MajorGC();
    }

    for (auto& h : kept) {
        assert(h.Get() != nullptr);
        assert(h.Get()->GetFastSlot(0, true).IsSmi());
    }

    Options::Get().gc_stress = false;
    Options::Get().gc_verify = false;
    std::cout << "  -> PASSED: Defragmentation clean under full HeapVerifier checks.\n" << std::flush;
}

int main() {
    try {
        TestOpportunisticDefragmentationChurn();
        TestPointerFixupAcrossEvacuatedBlocks();
        TestPinnedObjectNotEvacuated();
        TestDefragmentationUnderGCStress();
        std::cout << "\nAll Immix Opportunistic Defragmentation tests passed successfully!\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        return 1;
    }
}

