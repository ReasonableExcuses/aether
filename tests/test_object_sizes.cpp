#undef NDEBUG
#include <cassert>
#include <iostream>
#include <memory>
#include <string>

#include "aether/heap_object.h"
#include "aether/heap.h"
#include "aether/runtime.h"
#include "aether/shape.h"
#include "aether/memory_budget.h"

using namespace aether;

// Compile-time size invariants required by T1.7 per-object memory diet
static_assert(sizeof(HeapObject) == 24, "HeapObject size must be exactly 24 bytes");
static_assert(sizeof(JSObject) == 56, "JSObject size must be exactly 56 bytes (<= 64 B)");
static_assert(sizeof(JSArray) == 64, "JSArray size must be exactly 64 bytes (<= 64 B)");
static_assert(sizeof(JSFunction) == 64, "JSFunction size must be exactly 64 bytes (<= 64 B)");

void TestObjectInObjectStorage() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root_shape(0, nullptr, "", TaggedValue::Null());
    uint32_t sid = 1;

    JSObject obj(&root_shape);
    assert(obj.GetExtraData() == nullptr);
    assert(obj.ObjectSize() == sizeof(JSObject));

    auto state = CurrentMemoryBudgetState();
    const size_t initial_charge = state ? state->ChargedBytes() : 0;

    // Adding 1st property (in-object)
    obj.SetNamedProperty("a", TaggedValue::FromSmi(1), sid);
    assert(obj.GetExtraData() == nullptr);
    assert(obj.GetNamedProperty("a").AsSmi() == 1);
    if (state) {
        assert(state->ChargedBytes() == initial_charge);
    }

    // Adding 2nd property (in-object)
    obj.SetNamedProperty("b", TaggedValue::FromSmi(2), sid);
    assert(obj.GetExtraData() == nullptr);
    assert(obj.GetNamedProperty("b").AsSmi() == 2);
    if (state) {
        assert(state->ChargedBytes() == initial_charge);
    }

    // Both properties fit in-object: zero out-of-line allocations
    assert(obj.GetFastSlot(0, true).AsSmi() == 1);
    assert(obj.GetFastSlot(1, true).AsSmi() == 2);

    // Adding 3rd property spills out-of-line into extra data
    obj.SetNamedProperty("c", TaggedValue::FromSmi(3), sid);
    assert(obj.GetExtraData() != nullptr);
    assert(!obj.GetExtraData()->out_of_line_slots.empty());
    assert(obj.GetNamedProperty("c").AsSmi() == 3);
    assert(obj.GetFastSlot(2, false).AsSmi() == 3);

    // Verify all 3 properties are preserved
    assert(obj.GetNamedProperty("a").AsSmi() == 1);
    assert(obj.GetNamedProperty("b").AsSmi() == 2);
    assert(obj.GetNamedProperty("c").AsSmi() == 3);

    std::cout << "  -> In-object properties and out-of-line spill verified.\n";
}

void TestObjectDeletionAndReaddition() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root_shape(0, nullptr, "", TaggedValue::Null());
    uint32_t sid = 1;

    JSObject obj(&root_shape);
    obj.SetNamedProperty("x", TaggedValue::FromSmi(10), sid);
    obj.SetNamedProperty("y", TaggedValue::FromSmi(20), sid);

    assert(obj.HasOwnProperty("x"));
    assert(obj.DeleteNamedProperty("x"));
    assert(!obj.HasOwnProperty("x"));
    assert(obj.GetNamedProperty("x").IsUndefined());

    // Re-add "x"
    obj.SetNamedProperty("x", TaggedValue::FromSmi(99), sid);
    assert(obj.HasOwnProperty("x"));
    assert(obj.GetNamedProperty("x").AsSmi() == 99);
    assert(obj.GetNamedProperty("y").AsSmi() == 20);

    std::cout << "  -> In-object deletion and re-addition verified.\n";
}

void TestArrayMemoryDiet() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root_shape(0, nullptr, "", TaggedValue::Null());

    JSArray empty_array(&root_shape, 0);
    assert(empty_array.Length() == 0);
    assert(empty_array.ObjectSize() == sizeof(JSArray));

    JSArray array(&root_shape, 2);
    assert(array.Length() == 2);
    array.SetElement(0, TaggedValue::FromSmi(100));
    array.SetElement(1, TaggedValue::FromSmi(200));

    assert(array.GetElement(0).AsSmi() == 100);
    assert(array.GetElement(1).AsSmi() == 200);

    array.Push(TaggedValue::FromSmi(300));
    assert(array.Length() == 3);
    assert(array.GetElement(2).AsSmi() == 300);

    std::cout << "  -> Array memory diet and storage verified.\n";
}

void TestFunctionMemoryDiet() {
    Heap heap;
    IsolateScope isolate(heap);

    JSFunction fn("testFunc", nullptr);
    assert(fn.ObjectSize() == sizeof(JSFunction));
    assert(fn.Name() == "testFunc");
    assert(fn.GetConstructorPrototype().IsEmpty());

    TaggedValue proto = TaggedValue::FromHeapObject(AllocateRuntime<JSObject>(nullptr));
    fn.SetConstructorPrototype(proto);
    assert(fn.GetConstructorPrototype() == proto);

    std::cout << "  -> Function memory diet verified.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   AetherVM T1.7 Per-Object Memory Diet Test Suite\n";
    std::cout << "========================================================\n";

    TestObjectInObjectStorage();
    TestObjectDeletionAndReaddition();
    TestArrayMemoryDiet();
    TestFunctionMemoryDiet();

    std::cout << "\n[PASS] All per-object memory diet tests passed successfully.\n";
    return 0;
}
