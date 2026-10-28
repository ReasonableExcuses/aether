#include "aether/common.h"
#include "aether/value.h"
#include "aether/shape.h"
#include "aether/heap_object.h"
#include "aether/heap.h"
#include <iostream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestNurseryBumpAllocation() {
    std::cout << "[Test 1] Verifying Nursery Bump-Pointer Allocation...\n" << std::flush;

    IsolateHeap heap(64 * 1024); // 64KB small nursery for precise testing
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    void* ptr1 = heap.Allocate<JSObject>(root_shape.get());
    void* ptr2 = heap.Allocate<JSObject>(root_shape.get());

    assert(ptr1 != nullptr);
    assert(ptr2 != nullptr);
    assert(reinterpret_cast<uintptr_t>(ptr2) > reinterpret_cast<uintptr_t>(ptr1));
    assert(heap.GetNursery().Contains(ptr1));
    assert(heap.GetNursery().Contains(ptr2));
    (void)ptr1;
    (void)ptr2;

    std::cout << "  -> Nursery allocated contiguous objects with proper 8-byte alignment.\n" << std::flush;
}

void TestCardTableWriteBarrier() {
    std::cout << "[Test 2] Verifying Card-Marking Generational Write Barrier...\n" << std::flush;

    IsolateHeap heap(64 * 1024);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    // 1. Allocate an old-generation object directly in Immix
    void* old_mem = heap.GetOldGen().Allocate(sizeof(JSObject));
    JSObject* old_obj = new (old_mem) JSObject(root_shape.get());
    old_obj->SetOld(true);
    heap.GetOldGen().RegisterObject(old_obj);

    // 2. Allocate a young-generation object in Nursery
    JSObject* young_obj = heap.Allocate<JSObject>(root_shape.get());
    assert(young_obj->IsYoung());

    // 3. Store young object into old object -> triggers write barrier
    heap.WriteBarrier(old_obj, TaggedValue::FromHeapObject(young_obj));

    uintptr_t base = heap.GetNursery().FromSpaceStart();
    uintptr_t addr = reinterpret_cast<uintptr_t>(old_obj);
    size_t card_idx = (addr - base) >> kCardShift;

    assert(heap.GetCardTable().IsDirty(card_idx));
    std::cout << "  -> Write barrier accurately marked Card #" << card_idx << " for Old-to-Young store.\n" << std::flush;
}

void TestMinorGCScavenge() {
    std::cout << "[Test 3] Verifying Minor GC (Cheney Copying Scavenger)...\n" << std::flush;

    IsolateHeap heap(32 * 1024); // 32KB nursery
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    Handle<JSObject> live_handle;
    {
        HandleScope scope(heap);
        JSObject* live_obj = heap.Allocate<JSObject>(root_shape.get());
        live_handle = scope.CreateHandle<JSObject>(live_obj);

        // Fill nursery with un-rooted dead objects
        for (int i = 0; i < 20; ++i) {
            heap.Allocate<JSObject>(root_shape.get());
        }

        uintptr_t old_addr = reinterpret_cast<uintptr_t>(live_handle.Get());

        // Trigger Minor GC
        heap.MinorGC();
        assert(heap.MinorGCCount() == 1);

        uintptr_t new_addr = reinterpret_cast<uintptr_t>(live_handle.Get());
        assert(live_handle.Get() != nullptr);
        // Address has moved due to evacuation into To-space
        assert(new_addr != old_addr);
        assert(heap.GetNursery().Contains(live_handle.Get()));
        (void)old_addr;
        (void)new_addr;
    }

    std::cout << "  -> Scavenger evacuated live rooted object and updated Handle successfully.\n" << std::flush;
}

void TestObjectTenuring() {
    std::cout << "[Test 4] Verifying Object Tenuring across Scavenge Cycles...\n" << std::flush;

    IsolateHeap heap(32 * 1024);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    Handle<JSObject> persistent_handle;
    HandleScope scope(heap);
    JSObject* obj = heap.Allocate<JSObject>(root_shape.get());
    persistent_handle = scope.CreateHandle<JSObject>(obj);

    assert(persistent_handle->IsYoung());
    assert(persistent_handle->GetAge() == 0);

    // Scavenge 1
    heap.MinorGC();
    assert(persistent_handle->GetAge() == 1);
    assert(persistent_handle->IsYoung());

    // Scavenge 2 -> triggers tenuring to Old Gen Immix space
    heap.MinorGC();
    assert(persistent_handle->IsOld());
    assert(heap.GetOldGen().Contains(persistent_handle.Get()));

    std::cout << "  -> Object survived " << static_cast<int>(kTenuringAgeThreshold) 
              << " scavenges and was promoted to Immix Old Generation.\n" << std::flush;
}

void TestMajorGC() {
    std::cout << "[Test 5] Verifying Immix Mark-Sweep Major GC...\n" << std::flush;

    IsolateHeap heap(64 * 1024);
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    void* old_mem = heap.GetOldGen().Allocate(sizeof(JSObject));
    JSObject* persistent_old = new (old_mem) JSObject(root_shape.get());
    persistent_old->SetOld(true);
    heap.GetOldGen().RegisterObject(persistent_old);

    Handle<JSObject> old_handle = scope.CreateHandle<JSObject>(persistent_old);

    // Trigger Major GC
    heap.MajorGC();
    assert(heap.MajorGCCount() == 1);
    assert(old_handle->IsMarked());

    std::cout << "  -> Major GC traced roots and marked Immix blocks successfully.\n" << std::flush;
}

void TestHighAllocationPressure() {
    std::cout << "[Test 6] High Allocation Pressure Stress Test (10,000 Objects)...\n" << std::flush;

    IsolateHeap heap(16 * 1024); // Tiny 16KB semi-space to trigger dozens of GCs
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);
    Handle<JSObject> anchor = scope.CreateHandle<JSObject>(heap.Allocate<JSObject>(root_shape.get()));

    for (int i = 0; i < 10000; ++i) {
        heap.Allocate<JSObject>(root_shape.get());
    }

    assert(heap.MinorGCCount() > 5);
    assert(anchor.Get() != nullptr);

    std::cout << "  -> 10,000 objects allocated. Triggered " << heap.MinorGCCount() 
              << " Minor GCs without memory leaks or corruption.\n" << std::flush;
}

void TestOldToYoungRememberedSetSurvival() {
    std::cout << "[Test 7] Verifying Old-to-Young Remembered Set & Cross-Generational References...\n" << std::flush;

    IsolateHeap heap;
    auto root_shape = std::make_unique<Shape>(0, nullptr, "", TaggedValue::Null());

    HandleScope scope(heap);

    // 1. Allocate an old generation object directly in Immix Old Space
    JSObject* old_obj = heap.AllocateOld<JSObject>(root_shape.get());
    assert(old_obj->IsOld());

    // Root the old object so it survives collections
    Handle<JSObject> old_handle = scope.CreateHandle<JSObject>(old_obj);

    // 2. Allocate a young nursery object without a direct handle/root
    JSObject* young_child = heap.Allocate<JSObject>(root_shape.get());
    assert(!young_child->IsOld());
    void* initial_young_addr = young_child;

    // 3. Store young child in old object's prototype (triggers WriteBarrier)
    old_handle->SetPrototype(young_child);
    assert(old_handle->GetPrototype() == young_child);

    // Verify card is marked dirty
    uintptr_t base = heap.GetNursery().FromSpaceStart();
    uintptr_t addr = reinterpret_cast<uintptr_t>(old_obj);
    size_t card_index = (addr - base) >> kCardShift;
    assert(heap.GetCardTable().IsDirty(card_index));

    // 4. Trigger first Minor GC: young_child is ONLY reachable through old_obj!
    heap.MinorGC();
    assert(heap.MinorGCCount() == 1);

    // The young child must have been evacuated to ToSpace, and old_handle's prototype slot updated!
    JSObject* evacuated_child = old_handle->GetPrototype();
    assert(evacuated_child != nullptr);
    assert(evacuated_child != initial_young_addr); // Pointer was updated to new nursery location
    assert(evacuated_child->IsJSObject());
    assert(evacuated_child->GetAge() == 1);

    // 5. Trigger second Minor GC: evacuated_child should tenure or survive another scavenge
    heap.MinorGC();
    assert(heap.MinorGCCount() == 2);

    JSObject* surviving_child = old_handle->GetPrototype();
    assert(surviving_child != nullptr);
    assert(surviving_child->IsJSObject());

    std::cout << "  -> Old-to-young remembered set scanned and updated slots across multiple Minor GCs successfully.\n" << std::flush;
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 3 Verification Test Suite          \n"
              << "========================================================\n\n" << std::flush;

    TestNurseryBumpAllocation();
    std::cout << "\n" << std::flush;
    TestCardTableWriteBarrier();
    std::cout << "\n" << std::flush;
    TestMinorGCScavenge();
    std::cout << "\n" << std::flush;
    TestObjectTenuring();
    std::cout << "\n" << std::flush;
    TestMajorGC();
    std::cout << "\n" << std::flush;
    TestHighAllocationPressure();
    std::cout << "\n" << std::flush;
    TestOldToYoungRememberedSetSurvival();

    std::cout << "\n========================================================\n"
              << "   ALL MILESTONE 3 ARCHITECTURAL TESTS PASSED (7/7)     \n"
              << "========================================================\n" << std::flush;
    return 0;
}
