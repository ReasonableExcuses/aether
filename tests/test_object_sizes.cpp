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

// Compile-time size invariants required by T1.7 per-object memory diet & T2.1 GC array types
static_assert(sizeof(HeapObject) == 24, "HeapObject size must be exactly 24 bytes");
static_assert(sizeof(JSObject) == 56, "JSObject size must be exactly 56 bytes (<= 64 B)");
static_assert(sizeof(JSArray) == 64, "JSArray size must be exactly 64 bytes (<= 64 B)");
static_assert(sizeof(JSFunction) == 64, "JSFunction size must be exactly 64 bytes (<= 64 B)");
static_assert(sizeof(FixedArray) == 32, "FixedArray size must be exactly 32 bytes");
static_assert(sizeof(ByteArray) == 32, "ByteArray size must be exactly 32 bytes");
static_assert(sizeof(LexicalEnvironment) == 40, "LexicalEnvironment size must be exactly 40 bytes (<= 48 B)");
static_assert(sizeof(JSMap) == 64, "JSMap size must be exactly 64 bytes (<= 64 B)");
static_assert(sizeof(JSSet) == 64, "JSSet size must be exactly 64 bytes (<= 64 B)");
static_assert(sizeof(JSPromise) == 80, "JSPromise size must be exactly 80 bytes (<= 80 B)");
static_assert(sizeof(JSString) == 56, "JSString size must be exactly 56 bytes (<= 64 B)");

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
    assert(obj.GetExtraData()->out_of_line_slots != nullptr);
    assert(obj.GetExtraData()->out_of_line_slots->Length() > 0);
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
    assert(empty_array.ElementsArray() == nullptr);

    JSArray array(&root_shape, 2);
    assert(array.Length() == 2);
    assert(array.ElementsArray() != nullptr);
    assert(array.ElementsArray()->IsFixedArray());
    assert(array.ElementsArray()->Length() == 2);
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

    // Verify captured with_stack migration to GC-allocated FixedArray
    assert(!fn.HasWithStack());
    assert(fn.GetWithStackArray() == nullptr);
    assert(fn.GetWithStack().empty());

    auto* obj1 = AllocateRuntime<JSObject>(nullptr);
    auto* obj2 = AllocateRuntime<JSObject>(nullptr);
    fn.SetWithStack({obj1, obj2});
    assert(fn.HasWithStack());
    assert(fn.GetWithStackArray() != nullptr);
    assert(fn.GetWithStackArray()->Length() == 2);
    auto ws_vec = fn.GetWithStack();
    assert(ws_vec.size() == 2);
    assert(ws_vec[0] == obj1);
    assert(ws_vec[1] == obj2);

    // Verify captured instance_fields migration to GC-allocated FixedArray
    assert(!fn.HasInstanceFields());
    assert(fn.InstanceFieldsArray() == nullptr);
    assert(fn.InstanceFields().empty());

    fn.AddInstanceField("field1", TaggedValue::FromSmi(42), false);
    fn.AddInstanceField("method1", TaggedValue::Null(), true);
    assert(fn.HasInstanceFields());
    assert(fn.InstanceFieldsArray() != nullptr);
    assert(fn.InstanceFieldsArray()->Length() == 6); // 2 fields * 3 slots
    auto fields = fn.InstanceFields();
    assert(fields.size() == 2);
    assert(fields[0].name == "field1");
    assert(fields[0].initial_value.AsSmi() == 42);
    assert(!fields[0].is_method);
    assert(fields[1].name == "method1");
    assert(fields[1].initial_value.IsNull());
    assert(fields[1].is_method);

    std::cout << "  -> Function memory diet & captured FixedArray data verified.\n";
}

void TestGCAllocatedArrays() {
    Heap heap;
    IsolateScope isolate(heap);

    // FixedArray allocation & access
    FixedArray* fa = AllocateRuntimeFixedArray(5, 10, TaggedValue::FromSmi(42));
    assert(fa->Length() == 5);
    assert(fa->Capacity() == 10);
    assert(fa->GetType() == HeapObjectType::FixedArray);
    assert(fa->IsFixedArray());
    assert(fa->ObjectSize() == FixedArray::ComputeAllocationSize(10));
    assert(fa->Get(0).AsSmi() == 42);
    assert(fa->Get(4).AsSmi() == 42);
    fa->Set(2, TaggedValue::FromSmi(999));
    assert(fa->Get(2).AsSmi() == 999);

    // ByteArray allocation & access
    ByteArray* ba = AllocateRuntimeByteArray(64);
    assert(ba->Length() == 64);
    assert(ba->GetType() == HeapObjectType::ByteArray);
    assert(ba->IsByteArray());
    assert(ba->ObjectSize() == ByteArray::ComputeAllocationSize(64));
    ba->Set(10, 0xAB);
    assert(ba->Get(10) == 0xAB);

    std::cout << "  -> FixedArray and ByteArray GC allocation verified.\n";
}

void TestLexicalEnvironmentDiet() {
    std::cout << "[Test 6] Verifying LexicalEnvironment Memory Diet & GC Slots...\n";
    Heap heap;
    IsolateScope isolate(heap);

    // Empty environment requires zero auxiliary FixedArray allocation
    LexicalEnvironment empty_env(nullptr, 0);
    assert(empty_env.Outer() == nullptr);
    assert(empty_env.SlotCount() == 0);
    assert(empty_env.SlotsArray() == nullptr);
    assert(empty_env.ObjectSize() == sizeof(LexicalEnvironment));
    assert(empty_env.GetSlot(0).IsUndefined());
    assert(empty_env.Get(0, 0).IsUndefined());

    // Non-empty environment allocates FixedArray
    LexicalEnvironment env(nullptr, 4);
    assert(env.Outer() == nullptr);
    assert(env.SlotCount() == 4);
    assert(env.SlotsArray() != nullptr);
    assert(env.SlotsArray()->Length() == 4);
    assert(env.GetSlot(0).IsUndefined());
    assert(env.GetSlot(3).IsUndefined());

    env.SetSlot(0, TaggedValue::FromSmi(42));
    env.SetSlot(3, TaggedValue::FromDouble(3.1415));
    assert(env.GetSlot(0).AsSmi() == 42);
    assert(env.GetSlot(3).AsDouble() == 3.1415);

    // Nested lexical environment
    LexicalEnvironment nested(&env, 2);
    assert(nested.Outer() == &env);
    assert(nested.SlotCount() == 2);
    assert(nested.Get(0, 0).IsUndefined());
    assert(nested.Get(1, 0).AsSmi() == 42);
    assert(nested.Get(1, 3).AsDouble() == 3.1415);

    nested.Set(1, 0, TaggedValue::FromSmi(100));
    assert(env.GetSlot(0).AsSmi() == 100);

    std::cout << "  -> LexicalEnvironment memory diet & GC slots verified.\n";
}

void TestMapSetMemoryDiet() {
    Heap heap;
    IsolateScope isolate(heap);

    // JSMap memory diet and GC FixedArray table
    JSMap map(nullptr);
    assert(map.ObjectSize() == sizeof(JSMap));
    assert(map.Size() == 0);
    assert(map.Table() == nullptr);

    TaggedValue k1 = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("alpha"));
    TaggedValue v1 = TaggedValue::FromSmi(100);
    TaggedValue k2 = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("beta"));
    TaggedValue v2 = TaggedValue::FromSmi(200);

    map.Set(k1, v1);
    assert(map.Size() == 1);
    assert(map.Table() != nullptr);
    assert(map.Has(k1));
    assert(map.Get(k1).AsSmi() == 100);
    assert(!map.Has(k2));

    // Update existing key
    map.Set(k1, TaggedValue::FromSmi(999));
    assert(map.Size() == 1);
    assert(map.Get(k1).AsSmi() == 999);

    // Insert second key
    map.Set(k2, v2);
    assert(map.Size() == 2);
    assert(map.Has(k2));
    assert(map.Get(k2).AsSmi() == 200);
    assert(map.Entries().size() == 2);

    // Delete first key
    assert(map.Delete(k1));
    assert(map.Size() == 1);
    assert(!map.Has(k1));
    assert(map.Get(k1).IsUndefined());
    assert(map.Has(k2));

    // Clear map
    map.Clear();
    assert(map.Size() == 0);
    assert(map.Table() == nullptr);

    // JSSet memory diet and GC FixedArray table
    JSSet set(nullptr);
    assert(set.ObjectSize() == sizeof(JSSet));
    assert(set.Size() == 0);
    assert(set.Table() == nullptr);

    TaggedValue s1 = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("val1"));
    TaggedValue s2 = TaggedValue::FromHeapObject(AllocateRuntime<JSString>("val2"));

    set.Add(s1);
    assert(set.Size() == 1);
    assert(set.Table() != nullptr);
    assert(set.Has(s1));
    assert(!set.Has(s2));

    // Duplicate add is a no-op
    set.Add(s1);
    assert(set.Size() == 1);

    // Add second value
    set.Add(s2);
    assert(set.Size() == 2);
    assert(set.Has(s2));
    assert(set.Values().size() == 2);

    // Delete first value
    assert(set.Delete(s1));
    assert(set.Size() == 1);
    assert(!set.Has(s1));
    assert(set.Has(s2));

    // Clear set
    set.Clear();
    assert(set.Size() == 0);
    assert(set.Table() == nullptr);

    std::cout << "  -> JSMap & JSSet memory diet & GC tables verified.\n";
}

void TestPromiseMemoryDiet() {
    Heap heap;
    IsolateScope isolate(heap);
    MicrotaskQueue queue;

    // JSPromise memory diet and GC FixedArray reactions
    JSPromise promise(nullptr);
    assert(promise.ObjectSize() == sizeof(JSPromise));
    assert(promise.State() == PromiseState::Pending);
    assert(promise.Result().IsUndefined());
    assert(promise.ReactionCount() == 0);
    assert(promise.ReactionsArray() == nullptr);
    assert(!promise.HasReactions());

    // Add reactions
    TaggedValue fn1 = TaggedValue::FromSmi(1);
    TaggedValue fn2 = TaggedValue::FromSmi(2);
    JSPromise downstream(nullptr);

    promise.AddReaction(fn1, fn2, &downstream, queue);
    assert(promise.ReactionCount() == 1);
    assert(promise.HasReactions());
    assert(promise.ReactionsArray() != nullptr);
    assert(promise.ReactionsArray()->Length() == 3);
    assert(promise.ReactionsArray()->Get(0).AsSmi() == 1);
    assert(promise.ReactionsArray()->Get(1).AsSmi() == 2);
    assert(promise.ReactionsArray()->Get(2).AsHeapObject() == &downstream);

    // Add another reaction
    promise.AddReaction(TaggedValue::Null(), TaggedValue::Null(), nullptr, queue);
    assert(promise.ReactionCount() == 2);
    assert(promise.ReactionsArray()->Length() == 6);

    // Fulfill promise - reactions cleared to nullptr, state updated
    TaggedValue fulfill_val = TaggedValue::FromSmi(42);
    promise.Fulfill(fulfill_val, queue);
    assert(promise.State() == PromiseState::Fulfilled);
    assert(promise.Result().AsSmi() == 42);
    assert(promise.ReactionCount() == 0);
    assert(promise.ReactionsArray() == nullptr);
    assert(!promise.HasReactions());

    // Subsequent add reaction on settled promise enqueues microtask immediately
    size_t q_size_before = queue.Size();
    promise.AddReaction(fn1, fn2, nullptr, queue);
    assert(queue.Size() > q_size_before);
    assert(promise.ReactionCount() == 0);

    std::cout << "  -> JSPromise memory diet & GC reactions verified.\n";
}

void TestStringMemoryDiet() {
    Heap heap;
    IsolateScope isolate(heap);

    static_assert(sizeof(JSString) == 56, "sizeof(JSString) must be 56 bytes");

    // 1. Empty string
    JSString empty("");
    assert(empty.ObjectSize() == sizeof(JSString));
    assert(empty.Length() == 0);
    assert(empty.IsOneByte());
    assert(empty.Hash() != 0);
    assert(empty.Value() == "");
    assert(empty.View() == "");

    // 2. Flat Latin-1 (ASCII)
    JSString s1("hello");
    assert(s1.ObjectSize() == sizeof(JSString));
    assert(s1.Length() == 5);
    assert(s1.IsOneByte());
    assert(s1.View() == "hello");
    assert(s1.Value() == "hello");
    assert(s1.CharCodeAt(0) == 'h');
    assert(s1.CharCodeAt(4) == 'o');
    assert(s1.CharAt(1)->Value() == "e");

    // 3. Flat Latin-1 Extended (é = 0xE9 / 233, length = 1)
    JSString s_acc("\xC3\xA9"); // UTF-8 for 'é' (U+00E9)
    assert(s_acc.Length() == 1);
    assert(s_acc.IsOneByte());
    assert(s_acc.CharCodeAt(0) == 233);
    assert(s_acc.Value() == "\xC3\xA9");

    // 4. Flat UTF-16 (Emoji 😀: U+1F600 => surrogate pair 0xD83D, 0xDE00, length = 2)
    JSString s_emoji("\xF0\x9F\x98\x80"); // UTF-8 for 😀
    assert(s_emoji.Length() == 2);
    assert(!s_emoji.IsOneByte());
    assert(s_emoji.CharCodeAt(0) == 0xD83D); // High surrogate (55357)
    assert(s_emoji.CharCodeAt(1) == 0xDE00); // Low surrogate (56832)
    assert(s_emoji.Value() == "\xF0\x9F\x98\x80");

    // 5. Lone surrogate preserved
    uint16_t lone_surrogate = 0xD83D;
    JSString s_lone(&lone_surrogate, 1);
    assert(s_lone.Length() == 1);
    assert(s_lone.CharCodeAt(0) == 0xD83D);
    assert(s_lone.Value() == "\xED\xA0\xBD"); // 3-byte WTF-8

    // 6. Equals and Compare
    JSString s2("hello");
    JSString s3("world");
    assert(s1.Equals(&s2));
    assert(!s1.Equals(&s3));
    assert(s1.Compare(&s2) == 0);
    assert(s1.Compare(&s3) < 0);
    assert(s3.Compare(&s1) > 0);

    // 7. Cons Strings (Ropes) & Lazy Flattening
    auto* a = AllocateRuntime<JSString>("Aether");
    auto* b = AllocateRuntime<JSString>("VM");
    auto* rope = JSString::Concat(a, b);
    assert(rope->Length() == 8);
    assert(rope->CharCodeAt(0) == 'A');
    assert(rope->CharCodeAt(6) == 'V');
    assert(rope->CharCodeAt(7) == 'M');
    assert(rope->Value() == "AetherVM");

    // Deep rope concatenation & flattening
    auto* accum = AllocateRuntime<JSString>("");
    for (int i = 0; i < 40; ++i) {
        accum = JSString::Concat(accum, AllocateRuntime<JSString>("ab"));
    }
    assert(accum->Length() == 80);
    accum->EnsureFlat();
    assert(accum->IsOneByte());
    assert(accum->View().length() == 80);

    // 8. Sliced Strings
    auto* orig = AllocateRuntime<JSString>("Hello, Beautiful World!");
    auto* slice1 = JSString::Slice(orig, 7, 9); // "Beautiful"
    assert(slice1->Length() == 9);
    assert(slice1->CharCodeAt(0) == 'B');
    assert(slice1->CharCodeAt(8) == 'l');
    assert(slice1->Value() == "Beautiful");

    // Nested slice collapses to parent
    auto* slice2 = JSString::Slice(slice1, 4, 4); // "tifu"
    assert(slice2->Length() == 4);
    assert(slice2->Value() == "tifu");

    std::cout << "  -> JSString memory diet & UTF-16 representation verified.\n";
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   AetherVM T1.7 Per-Object Memory Diet Test Suite\n";
    std::cout << "========================================================\n";

    TestObjectInObjectStorage();
    TestObjectDeletionAndReaddition();
    TestArrayMemoryDiet();
    TestFunctionMemoryDiet();
    TestGCAllocatedArrays();
    TestLexicalEnvironmentDiet();
    TestMapSetMemoryDiet();
    TestPromiseMemoryDiet();
    TestStringMemoryDiet();

    std::cout << "\n[PASS] All per-object memory diet tests passed successfully.\n";
    return 0;
}

