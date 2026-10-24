#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/typed_array.h"

#include <array>
#include <iostream>
#include <limits>
#include <optional>

using namespace aether;

namespace {

template <typename F>
void ExpectLengthError(F&& operation) {
    bool threw = false;
    try { operation(); }
    catch (const std::length_error&) { threw = true; }
    assert(threw);
}

struct BudgetLimit {
    explicit BudgetLimit(size_t limit)
        : state(CurrentMemoryBudgetState()), previous(state->TotalLimit()) {
        state->SetTotalLimit(limit);
    }
    ~BudgetLimit() { state->SetTotalLimit(previous); }
    std::shared_ptr<MemoryBudgetState> state;
    size_t previous;
};

// Stable native slots keep this suite independent of HandleScope's container
// representation, including while the heap/GC hardening is integrated.
template <typename T>
class Root {
public:
    explicit Root(T* object) : heap_(*IsolateHeap::Current()), value_(TaggedValue::FromHeapObject(object)) {
        heap_.RegisterExtraRoot(&value_);
    }
    ~Root() { heap_.UnregisterExtraRoot(&value_); }
    Root(const Root&) = delete;
    T* Get() const { return value_.As<T>(); }
    T* operator->() const { return Get(); }
private:
    Heap& heap_;
    TaggedValue value_;
};

void TestStringLimitsAndCharges() {
    Heap heap;
    IsolateScope isolate(heap);
    auto state = CurrentMemoryBudgetState();
    const size_t baseline = state->ChargedBytes();
    assert(CheckedStringLength(kMaxStringLength) == kMaxStringLength);
    ExpectLengthError([] { CheckedStringLength(kMaxStringLength + 1); });
    ExpectLengthError([] { CheckedStringLength(std::numeric_limits<size_t>::max()); });
    ExpectLengthError([] { CheckedStorageBytes(std::numeric_limits<size_t>::max(), 8); });

    // A real small input verifies rejection before its backing copy is made.
    const std::string text(4096, 'x');
    {
        BudgetLimit limit(baseline + 128);
        ExpectLengthError([&] { JSString rejected(text); });
        assert(state->ChargedBytes() == baseline);
    }
    {
        {
            JSError startup_error("Error", "allocator regression");
            assert(startup_error.Name() == "Error");
        }
        heap.CollectAtSafepoint();
        assert(state->ChargedBytes() == baseline);
        JSString value(text);
        const size_t charged = state->ChargedBytes();
        assert(charged - baseline == value.Value().capacity() + 1);
        JSString moved(std::move(value));
        assert(moved.View() == text);
        assert(state->ChargedBytes() == charged);
    }
    assert(state->ChargedBytes() == baseline);
}

void TestArrayLimitsAndAtomicFailure() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root(0, nullptr, "", TaggedValue::Null());
    auto state = CurrentMemoryBudgetState();
    ExpectLengthError([&] { JSArray rejected(&root, kMaxArrayLength + 1); });
    ExpectLengthError([&] { JSArray rejected(&root, std::numeric_limits<size_t>::max()); });

    JSArray array(&root, 3);
    array.SetElement(0, TaggedValue::FromSmi(17));
    const size_t before = state->ChargedBytes();
    const auto* data = array.Elements().data();
    ExpectLengthError([&] { array.SetLength(kMaxArrayLength + 1); });
    ExpectLengthError([&] { array.SetElement(kMaxArrayLength, TaggedValue::True()); });
    ExpectLengthError([&] { array.SetElement(std::numeric_limits<size_t>::max(), TaggedValue::True()); });
    ExpectLengthError([&] { array.ReserveElements(kMaxArrayLength + 1); });
    {
        BudgetLimit limit(before + 1);
        ExpectLengthError([&] { array.SetLength(100); });
        array.SetLength(array.Elements().capacity());
        ExpectLengthError([&] { array.Push(TaggedValue::True()); });
        array.SetLength(3);
    }
    assert(array.Length() == 3 && array.GetElement(0).AsSmi() == 17);
    assert(array.Elements().data() == data);
    assert(state->ChargedBytes() == before);
    uint32_t sid = 1;
    ExpectLengthError([&] { array.SetNamedProperty("length", TaggedValue::FromDouble(3.5), sid); });
    ExpectLengthError([&] { array.SetNamedProperty("length", TaggedValue::FromSmi(-1), sid); });
    ExpectLengthError([&] { array.SetNamedProperty("184467440737095516160000", TaggedValue::True(), sid); });
    assert(array.Length() == 3);
    array.SetNamedProperty("length", TaggedValue::FromDouble(4), sid);
    assert(array.Length() == 4 && array.GetElement(3).IsUndefined());
    const size_t grown_charge = state->ChargedBytes();
    array.SetLength(0);
    assert(state->ChargedBytes() == grown_charge); // retained capacity is still owned
}

void TestPropertyFailureAndSafeReads() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root(0, nullptr, "", TaggedValue::Null());
    JSObject object(&root);
    uint32_t sid = 1;
    for (size_t i = 0; i < Shape::kDefaultInObjectProperties; ++i) {
        object.SetNamedProperty(std::to_string(i), TaggedValue::FromSmi(static_cast<int32_t>(i)), sid);
    }
    auto* before_shape = object.GetShape();
    const auto before_sid = sid;
    auto state = CurrentMemoryBudgetState();
    const size_t before = state->ChargedBytes();
    {
        BudgetLimit limit(before);
        ExpectLengthError([&] { object.SetNamedProperty("fifth", TaggedValue::True(), sid); });
        ExpectLengthError([&] { object.SetAccessor("getter", TaggedValue::Undefined(), TaggedValue::Undefined()); });
        assert(object.GetShape() == before_shape && sid == before_sid);
        assert(!object.HasOwnProperty("fifth") && !object.HasAccessor("getter"));
        assert(state->ChargedBytes() == before);
    }
    ExpectLengthError([&] { object.SetFastSlot(static_cast<uint32_t>(kMaxObjectProperties), false, TaggedValue::True()); });
    ExpectLengthError([&] { object.SetFastSlot(UINT32_MAX, false, TaggedValue::True()); });
    ExpectLengthError([&] { object.SetFastSlot(Shape::kDefaultInObjectProperties, true, TaggedValue::True()); });
    ExpectLengthError([&] { object.SetFastSlot(0, false, TaggedValue::True()); });
    assert(object.GetFastSlot(UINT32_MAX, false).IsEmpty());
    assert(object.GetFastSlot(UINT32_MAX, true).IsEmpty());
    assert(object.GetFastSlot(0, false).IsEmpty());
    assert(object.GetFastSlot(Shape::kDefaultInObjectProperties, false).IsEmpty());

    object.SetNamedProperty("fifth", TaggedValue::FromSmi(42), sid);
    assert(object.GetNamedProperty("fifth").AsSmi() == 42);
    assert(object.DeleteNamedProperty("fifth"));
    const size_t deleted_charge = state->ChargedBytes();
    {
        BudgetLimit limit(deleted_charge);
        ExpectLengthError([&] { object.SetNamedProperty("fifth", TaggedValue::True(), sid); });
        assert(object.IsDeletedProperty("fifth"));
        assert(!object.HasOwnProperty("fifth"));
    }
    object.SetNamedProperty("fifth", TaggedValue::True(), sid);
    assert(object.HasOwnProperty("fifth") && !object.IsDeletedProperty("fifth"));
}

void TestPrototypeGuards() {
    Heap heap;
    IsolateScope isolate(heap);
    JSObject a(nullptr), b(nullptr), c(nullptr);
    assert(!a.SetPrototype(&a));
    assert(a.SetPrototype(&b));
    assert(!b.SetPrototypeSafe(&a));
    assert(b.GetPrototype() == nullptr);
    assert(b.SetPrototype(&c));
    assert(!c.SetPrototype(&a));
    a.PreventExtensions();
    assert(a.SetPrototype(&b));
    assert(!a.SetPrototype(nullptr));
    assert(a.GetPrototype() == &b);

    std::vector<std::unique_ptr<JSObject>> chain;
    for (size_t i = 0; i <= kMaxPrototypeChainDepth + 1; ++i) {
        chain.push_back(std::make_unique<JSObject>(nullptr));
        if (i == 0) continue;
        const bool result = chain[i]->SetPrototype(chain[i - 1].get());
        assert(result == (i <= kMaxPrototypeChainDepth));
    }
    Shape cyclic_shape(0, nullptr, "", TaggedValue::FromHeapObject(&c));
    JSObject via_shape(&cyclic_shape);
    assert(!c.SetPrototype(&via_shape));
    JSObject moved(std::move(a));
    assert(!moved.IsExtensible() && moved.GetPrototype() == &b);
}

void TestBufferAndViewBounds() {
    Heap heap;
    IsolateScope isolate(heap);
    const size_t huge = std::numeric_limits<size_t>::max();
    ExpectLengthError([] { JSArrayBuffer rejected(kMaxArrayBufferSize + 1); });
    ExpectLengthError([&] { JSArrayBuffer rejected(huge); });
    ExpectLengthError([] { JSArrayBuffer rejected(8, 4, true); });
    ExpectLengthError([] { JSArrayBuffer rejected(8, 0, true); });
    ExpectLengthError([] { JSArrayBuffer rejected(size_t{0}, kMaxArrayBufferSize + 1, true); });
    ExpectLengthError([] { JSArrayBuffer rejected(static_cast<uint8_t*>(nullptr), 1); });
    std::array<uint8_t, 16> external{};
    ExpectLengthError([&] { JSArrayBuffer rejected(external.data(), kMaxArrayBufferSize + 1); });
    JSArrayBuffer buffer(external.data(), external.size());
    JSTypedArray typed(TypedArrayKind::Uint32, &buffer, 4, 3);
    typed.SetElement(2, TaggedValue::FromSmi(123));
    assert(typed.GetElement(2).AsSmi() == 123);
    assert(typed.GetElement(huge).IsUndefined());
    typed.SetElement(huge, TaggedValue::True());
    ExpectLengthError([&] { JSTypedArray rejected(TypedArrayKind::Uint32, &buffer, 1, 1); });
    ExpectLengthError([&] { JSTypedArray rejected(TypedArrayKind::Float64, &buffer, 0, huge / 8 + 1); });
    ExpectLengthError([&] { JSTypedArray rejected(TypedArrayKind::Uint8, &buffer, huge, 0); });
    ExpectLengthError([&] { JSTypedArray rejected(TypedArrayKind::Uint32, &buffer, 4, 4); });
    ExpectLengthError([] { JSTypedArray rejected(TypedArrayKind::Uint8, nullptr, 0, 0); });
    JSTypedArray empty(TypedArrayKind::Uint32, &buffer, 16, 0);
    assert(empty.Length() == 0);

    JSDataView view(&buffer, 3, 13);
    view.SetUint32(9, 0x12345678, true);
    assert(view.GetUint32(9, true) == 0x12345678);
    assert(!view.CheckBounds(huge, 1));
    assert(!view.CheckBounds(0, huge));
    assert(!view.CheckBounds(10, 4));
    view.SetUint32(huge, 0);
    assert(view.GetUint32(huge) == 0);
    ExpectLengthError([&] { JSDataView rejected(&buffer, huge, 1); });
    ExpectLengthError([&] { JSDataView rejected(&buffer, 1, huge); });
    ExpectLengthError([&] { JSDataView rejected(&buffer, 10, 7); });
    ExpectLengthError([] { JSDataView rejected(nullptr, 0, 0); });
    buffer.Detach();
    assert(typed.Length() == 0 && typed.Data() == nullptr);
    assert(!view.CheckBounds(0, 1));
    ExpectLengthError([&] { JSTypedArray rejected(TypedArrayKind::Uint8, &buffer, 0, 0); });
    ExpectLengthError([&] { JSDataView rejected(&buffer, 0, 0); });
    ExpectLengthError([&] { buffer.Slice(0, 0); });
    ExpectLengthError([&] { typed.Subarray(0, 0); });
}

void TestBufferChargesAndRelocation() {
    Heap heap;
    IsolateScope isolate(heap);
    auto state = CurrentMemoryBudgetState();
    const size_t baseline = state->ChargedBytes();
    const size_t buffers_before = state->ArrayBufferBytes();
    {
        std::optional<JSArrayBuffer> original(std::in_place, 8, 32, false);
        assert(state->ArrayBufferBytes() == buffers_before + 32);
        original->Data()[0] = 77;
        assert(original->Grow(32));
        assert(!original->Grow(33));
        const size_t charged = state->ChargedBytes();
        alignas(JSArrayBuffer) std::byte storage[sizeof(JSArrayBuffer)];
        auto* moved = static_cast<JSArrayBuffer*>(original->RelocateTo(storage));
        original.reset();
        assert(state->ChargedBytes() == charged);
        assert(moved->Data()[0] == 77 && moved->ByteLength() == 32);
        moved->Detach();
        assert(state->ArrayBufferBytes() == buffers_before);
        moved->~JSArrayBuffer();
    }
    assert(state->ChargedBytes() == baseline);
    size_t empty_object_charge;
    {
        JSArrayBuffer empty(size_t{0});
        empty_object_charge = state->ChargedBytes() - baseline;
    }
    {
        JSArrayBuffer first(64);
        const size_t charged = state->ChargedBytes();
        BudgetLimit limit(charged + empty_object_charge + 32);
        ExpectLengthError([] { JSArrayBuffer second(64); });
        assert(state->ChargedBytes() == charged);
        assert(state->ArrayBufferBytes() == buffers_before + 64);
    }
    assert(state->ChargedBytes() == baseline);
}

void TestCollectionRelocationAndBudgetOwnership() {
    Heap heap;
    IsolateScope isolate(heap);
    auto state = CurrentMemoryBudgetState();
    const size_t baseline = state->ChargedBytes();
    Shape root(0, nullptr, "", TaggedValue::Null());
    {
        std::optional<JSArray> original(std::in_place, &root, 16);
        original->SetElement(1, TaggedValue::FromSmi(99));
        uint32_t sid = 1;
        const std::string key(128, 'k');
        original->SetNamedProperty(key, TaggedValue::FromSmi(21), sid);
        {
            Heap second;
            IsolateScope second_isolate(second);
            auto second_state = CurrentMemoryBudgetState();
            const size_t second_before = second_state->ChargedBytes();
            const size_t first_before = state->ChargedBytes();
            original->SetLength(64);
            original->SetNamedProperty(std::string(256, 'q'), TaggedValue::True(), sid);
            assert(state->ChargedBytes() > first_before);
            assert(second_state->ChargedBytes() == second_before);
        }
        // Heap construction/destruction owns the thread-local current heap;
        // restore the outer isolate before continuing this test.
        IsolateScope restore_outer(heap);
        original->PreventExtensions();
        const size_t before_move = state->ChargedBytes();
        alignas(JSArray) std::byte storage[sizeof(JSArray)];
        auto* moved = static_cast<JSArray*>(original->RelocateTo(storage));
        original.reset();
        assert(state->ChargedBytes() == before_move);
        assert(moved->Length() == 64 && moved->GetElement(1).AsSmi() == 99);
        assert(moved->GetNamedProperty(key).AsSmi() == 21 && !moved->IsExtensible());
        moved->~JSArray();
    }
    assert(state->ChargedBytes() == baseline);
    {
        JSMap map(&root);
        JSSet set(&root);
        const size_t before = state->ChargedBytes();
        BudgetLimit limit(before);
        ExpectLengthError([&] { map.Set(TaggedValue::True(), TaggedValue::False()); });
        ExpectLengthError([&] { set.Add(TaggedValue::True()); });
        assert(map.Size() == 0 && set.Size() == 0);
        assert(state->ChargedBytes() == before);
    }
    assert(state->ChargedBytes() == baseline);
}

template <typename T>
void CheckManagedSize(const T& object) { assert(object.ObjectSize() == sizeof(T)); }

void TestManagedSizesAndCollectionCharges() {
    Heap heap;
    IsolateScope isolate(heap);
    Shape root(0, nullptr, "", TaggedValue::Null());
    JSString string("string"); JSSymbol symbol("symbol", 1); JSObject object(&root);
    JSArray array(&root, 32); JSMap map(&root); JSSet set(&root); JSPromise promise(&root);
    JSRegExp regexp("a+", ""); LexicalEnvironment environment(nullptr, 32);
    JSFunction function("f", static_cast<const BytecodeArray*>(nullptr));
    JSGeneratorObject generator(&function, {}, TaggedValue::Undefined(), nullptr);
    JSError error("Error", "test"); JSProxy proxy(TaggedValue::Null(), TaggedValue::Null());
    JSBigInt bigint(123); JSArrayBuffer buffer(32);
    JSTypedArray typed(TypedArrayKind::Uint8, &buffer, 0, 32); JSDataView view(&buffer, 0, 32);
    MicrotaskQueue queue;
    promise.AddReaction(TaggedValue::Undefined(), TaggedValue::Undefined(), nullptr, queue);
    generator.SavedRegisters().resize(32);
    map.Set(TaggedValue::True(), TaggedValue::FromSmi(1)); set.Add(TaggedValue::True());
    CheckManagedSize(string); CheckManagedSize(symbol); CheckManagedSize(object);
    CheckManagedSize(array); CheckManagedSize(map); CheckManagedSize(set); CheckManagedSize(promise);
    CheckManagedSize(regexp); CheckManagedSize(environment); CheckManagedSize(function);
    CheckManagedSize(generator); CheckManagedSize(error); CheckManagedSize(proxy); CheckManagedSize(bigint);
    CheckManagedSize(buffer); CheckManagedSize(typed); CheckManagedSize(view);
    auto state = CurrentMemoryBudgetState();
    const size_t before_clear = state->ChargedBytes();
    map.Clear(); set.Clear();
    assert(state->ChargedBytes() < before_clear);
}

void TestManagedHelpers() {
    Heap heap;
    IsolateScope isolate(heap);
    auto state = CurrentMemoryBudgetState();
    Root buffer(AllocateRuntime<JSArrayBuffer>(16));
    for (size_t i = 0; i < 16; ++i) buffer->Data()[i] = static_cast<uint8_t>(i);
    const size_t before_slice = state->ManagedBytes();
    Root sliced(buffer->Slice(INT64_MIN, INT64_MAX));
    assert(state->ManagedBytes() >= before_slice + sizeof(JSArrayBuffer));
    Root tail(buffer->Slice(-4, INT64_MAX));
    Root typed(AllocateRuntime<JSTypedArray>(TypedArrayKind::Uint16, buffer.Get(), 4, 6));
    const size_t before_subarray = state->ManagedBytes();
    Root sub(typed->Subarray(-2, INT64_MAX));
    assert(state->ManagedBytes() >= before_subarray + sizeof(JSTypedArray));
    Root empty(typed->Subarray(INT64_MAX, INT64_MIN));
    assert(sliced->ByteLength() == 16 && sliced->Data()[15] == 15);
    assert(tail->ByteLength() == 4 && tail->Data()[0] == 12);
    assert(sub->Length() == 2 && sub->ByteOffset() == 12);
    assert(empty->Length() == 0 && empty->ByteOffset() == 16);

    JSBigInt two(2), ten(10), minus_two(-2), three(3), five(5), zero(0);
    const size_t before_power = state->ManagedBytes();
    Root power(two.Exponentiate(ten));
    assert(state->ManagedBytes() >= before_power + sizeof(JSBigInt));
    Root shift(ten.ShiftLeft(minus_two));
    Root opposite_shift(ten.ShiftRight(minus_two));
    Root add(ten.Add(five));
    Root subtract(two.Subtract(five));
    Root multiply(ten.Multiply(five));
    Root divide(ten.Divide(three));
    Root remainder(ten.Remainder(three));
    Root negate(ten.Negate());
    Root bitnot(ten.BitwiseNot());
    Root bit_and(ten.BitwiseAnd(five));
    Root bit_or(ten.BitwiseOr(five));
    Root bitxor(ten.BitwiseXor(five));
    assert(power->Value() == 1024 && shift->Value() == 2 && opposite_shift->Value() == 40);
    assert(add->Value() == 15 && subtract->Value() == -3 && multiply->Value() == 50);
    assert(divide->Value() == 3 && remainder->Value() == 1);
    assert(negate->Value() == -10 && bitnot->Value() == -11);
    assert(bit_and->Value() == 0 && bit_or->Value() == 15 && bitxor->Value() == 15);
    assert(ten.Divide(zero) == nullptr && ten.Exponentiate(minus_two) == nullptr);
    heap.MinorGC();
    heap.MajorGC();
    assert(sliced->Data()[15] == 15 && sub->Buffer() == buffer.Get());
    assert(power->Value() == 1024 && bitnot->Value() == -11);
}

} // namespace

int main() {
    TestStringLimitsAndCharges();
    TestArrayLimitsAndAtomicFailure();
    TestPropertyFailureAndSafeReads();
    TestPrototypeGuards();
    TestBufferAndViewBounds();
    TestBufferChargesAndRelocation();
    TestCollectionRelocationAndBudgetOwnership();
    TestManagedSizesAndCollectionCharges();
    TestManagedHelpers();
    std::cout << "[PASS] Collection, string, buffer, prototype and managed-size hardening\n";
}
