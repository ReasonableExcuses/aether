#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/typed_array.h"
#include "aether/heap.h"

#include <iostream>
#include <vector>
#include <cmath>

using namespace aether;

void TestArrayBufferAllocationAndSlicing() {
    std::cout << "[Test 1] Verifying ArrayBuffer Allocation, ByteLength, and Slicing...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();

    // Direct C++ instantiation
    auto* ab = AllocateRuntime<JSArrayBuffer>(32, realm.GetRootShape());
    assert(ab->ByteLength() == 32);
    assert(!ab->IsDetached());
    assert(ab->Data() != nullptr);

    // Verify zero initialization
    for (size_t i = 0; i < 32; ++i) {
        assert(ab->Data()[i] == 0);
    }

    // Populate buffer with byte pattern
    for (size_t i = 0; i < 32; ++i) {
        ab->Data()[i] = static_cast<uint8_t>(i * 3);
    }

    // Slice buffer
    auto* sliced = ab->Slice(8, 24, realm.GetRootShape());
    assert(sliced->ByteLength() == 16);
    for (size_t i = 0; i < 16; ++i) {
        assert(sliced->Data()[i] == static_cast<uint8_t>((8 + i) * 3));
    }

    // Slice with negative indices
    auto* sliced_tail = ab->Slice(-8, 32, realm.GetRootShape());
    assert(sliced_tail->ByteLength() == 8);
    for (size_t i = 0; i < 8; ++i) {
        assert(sliced_tail->Data()[i] == static_cast<uint8_t>((24 + i) * 3));
    }

    std::cout << "  -> ArrayBuffer allocation, zero-init, and slice boundary semantics verified.\n" << std::flush;
}

void TestTypedArrayCreationAndIndexing() {
    std::cout << "[Test 2] Verifying TypedArray Creation, Alignment, and Element Indexing...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();

    // 1. Uint8Array
    auto* ab_u8 = AllocateRuntime<JSArrayBuffer>(8, realm.GetRootShape());
    auto* u8 = AllocateRuntime<JSTypedArray>(TypedArrayKind::Uint8, ab_u8, 0, 8, realm.GetRootShape());
    assert(u8->Length() == 8);
    assert(u8->ByteLength() == 8);
    assert(u8->BytesPerElement() == 1);

    for (size_t i = 0; i < 8; ++i) {
        u8->SetElement(i, TaggedValue::FromSmi(static_cast<int32_t>((i + 1) * 10)));
    }
    for (size_t i = 0; i < 8; ++i) {
        TaggedValue v = u8->GetElement(i);
        assert(v.IsSmi());
        assert(v.AsSmi() == static_cast<int32_t>((i + 1) * 10));
    }

    // 2. Int32Array
    auto* ab_i32 = AllocateRuntime<JSArrayBuffer>(16, realm.GetRootShape());
    auto* i32 = AllocateRuntime<JSTypedArray>(TypedArrayKind::Int32, ab_i32, 0, 4, realm.GetRootShape());
    assert(i32->Length() == 4);
    assert(i32->ByteLength() == 16);
    assert(i32->BytesPerElement() == 4);

    i32->SetElement(0, TaggedValue::FromSmi(-1000));
    i32->SetElement(1, TaggedValue::FromSmi(42));
    i32->SetElement(2, TaggedValue::FromSmi(99999));
    i32->SetElement(3, TaggedValue::FromSmi(-1));

    assert(i32->GetElement(0).AsSmi() == -1000);
    assert(i32->GetElement(1).AsSmi() == 42);
    assert(i32->GetElement(2).AsSmi() == 99999);
    assert(i32->GetElement(3).AsSmi() == -1);

    // 3. Float64Array
    auto* ab_f64 = AllocateRuntime<JSArrayBuffer>(24, realm.GetRootShape());
    auto* f64 = AllocateRuntime<JSTypedArray>(TypedArrayKind::Float64, ab_f64, 0, 3, realm.GetRootShape());
    assert(f64->Length() == 3);
    assert(f64->BytesPerElement() == 8);

    f64->SetElement(0, TaggedValue::FromDouble(3.141592653589793));
    f64->SetElement(1, TaggedValue::FromDouble(-0.0005));
    f64->SetElement(2, TaggedValue::FromDouble(1e12));

    assert(std::abs(f64->GetElement(0).AsDouble() - 3.141592653589793) < 1e-12);
    assert(std::abs(f64->GetElement(1).AsDouble() - (-0.0005)) < 1e-12);
    assert(f64->GetElement(2).AsDouble() == 1e12);

    // 4. Uint8ClampedArray (verify ECMA-262 rounding and clamping)
    auto* ab_clamped = AllocateRuntime<JSArrayBuffer>(4, realm.GetRootShape());
    auto* clamped = AllocateRuntime<JSTypedArray>(TypedArrayKind::Uint8Clamped, ab_clamped, 0, 4, realm.GetRootShape());

    clamped->SetElement(0, TaggedValue::FromDouble(-10.5)); // clamped to 0
    clamped->SetElement(1, TaggedValue::FromDouble(300.0)); // clamped to 255
    clamped->SetElement(2, TaggedValue::FromDouble(12.4));  // rounded to 12
    clamped->SetElement(3, TaggedValue::FromDouble(12.6));  // rounded to 13

    assert(clamped->GetElement(0).AsSmi() == 0);
    assert(clamped->GetElement(1).AsSmi() == 255);
    assert(clamped->GetElement(2).AsSmi() == 12);
    assert(clamped->GetElement(3).AsSmi() == 13);

    std::cout << "  -> Uint8Array, Int32Array, Float64Array, and Uint8ClampedArray validated successfully.\n" << std::flush;
}

void TestArrayBufferSharingAndSubarrays() {
    std::cout << "[Test 3] Verifying Multi-View Buffer Sharing and Subarrays...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();

    // 16-byte shared buffer
    auto* shared_buf = AllocateRuntime<JSArrayBuffer>(16, realm.GetRootShape());

    // View 1: Int32Array (4 elements)
    auto* view_i32 = AllocateRuntime<JSTypedArray>(TypedArrayKind::Int32, shared_buf, 0, 4, realm.GetRootShape());

    // View 2: Uint8Array (16 elements over the exact same buffer)
    auto* view_u8 = AllocateRuntime<JSTypedArray>(TypedArrayKind::Uint8, shared_buf, 0, 16, realm.GetRootShape());

    // Write through Int32Array
    view_i32->SetElement(0, TaggedValue::FromSmi(0x04030201));

    // Verify raw little-endian bytes visible through Uint8Array view
    assert(view_u8->GetElement(0).AsSmi() == 0x01);
    assert(view_u8->GetElement(1).AsSmi() == 0x02);
    assert(view_u8->GetElement(2).AsSmi() == 0x03);
    assert(view_u8->GetElement(3).AsSmi() == 0x04);

    // Mutate through Uint8Array
    view_u8->SetElement(0, TaggedValue::FromSmi(0xFF));
    assert(view_i32->GetElement(0).AsSmi() == static_cast<int32_t>(0x040302FF));

    // Subarray creation: view_u8.subarray(4, 8)
    auto* sub = view_u8->Subarray(4, 8, realm.GetRootShape());
    assert(sub->Length() == 4);
    assert(sub->ByteOffset() == 4);
    assert(sub->Buffer() == shared_buf);

    sub->SetElement(0, TaggedValue::FromSmi(0xAA));
    assert(view_u8->GetElement(4).AsSmi() == 0xAA);

    std::cout << "  -> Multi-view buffer aliasing and subarray offset sharing verified.\n" << std::flush;
}

void TestDataViewEndianness() {
    std::cout << "[Test 4] Verifying DataView Endianness-Sensitive Operations...\n" << std::flush;

    Agent agent;
    Realm& realm = agent.GetRealm();

    auto* buf = AllocateRuntime<JSArrayBuffer>(16, realm.GetRootShape());
    auto* dv = AllocateRuntime<JSDataView>(buf, 0, 16, realm.GetRootShape());

    // 16-bit integers
    dv->SetInt16(0, static_cast<int16_t>(0x1234), false); // Big-endian: [0x12, 0x34]
    assert(buf->Data()[0] == 0x12);
    assert(buf->Data()[1] == 0x34);
    assert(dv->GetInt16(0, false) == static_cast<int16_t>(0x1234));
    assert(dv->GetInt16(0, true) == static_cast<int16_t>(0x3412));

    dv->SetInt16(2, static_cast<int16_t>(0x1234), true); // Little-endian: [0x34, 0x12]
    assert(buf->Data()[2] == 0x34);
    assert(buf->Data()[3] == 0x12);
    assert(dv->GetInt16(2, true) == static_cast<int16_t>(0x1234));

    // 32-bit integers
    dv->SetInt32(4, 0x01020304, false); // Big-endian
    assert(buf->Data()[4] == 0x01);
    assert(buf->Data()[5] == 0x02);
    assert(buf->Data()[6] == 0x03);
    assert(buf->Data()[7] == 0x04);
    assert(dv->GetInt32(4, false) == 0x01020304);

    dv->SetInt32(8, 0x01020304, true); // Little-endian
    assert(buf->Data()[8] == 0x04);
    assert(buf->Data()[9] == 0x03);
    assert(buf->Data()[10] == 0x02);
    assert(buf->Data()[11] == 0x01);
    assert(dv->GetInt32(8, true) == 0x01020304);

    // 64-bit floats
    dv->SetFloat64(0, 12345.6789, true); // Little-endian
    assert(std::abs(dv->GetFloat64(0, true) - 12345.6789) < 1e-9);

    std::cout << "  -> DataView little-endian and big-endian encoding verified.\n" << std::flush;
}

void TestTypedArrayHigherOrderMethods() {
    std::cout << "[Test 5] Verifying TypedArray Functional & Mutator Prototype Methods...\n" << std::flush;

    Agent agent;

    // 1. fill & reverse
    std::string script1 = R"(
        let a = new Uint8Array(5);
        a.fill(7);
        let allSeven = true;
        for (let i = 0; i < 5; i++) {
            if (a[i] !== 7) allSeven = false;
        }
        allSeven;
    )";
    TaggedValue r1 = agent.EvaluateScript(script1);
    assert(r1.IsBoolean() && r1.AsBoolean());

    // 2. set & slice
    std::string script2 = R"(
        let src = new Int32Array(3);
        src[0] = 10; src[1] = 20; src[2] = 30;
        let dst = new Int32Array(5);
        dst.set(src, 1);
        let ok = dst[0] === 0 && dst[1] === 10 && dst[2] === 20 && dst[3] === 30 && dst[4] === 0;
        let sl = dst.slice(1, 4);
        ok = ok && sl.length === 3 && sl[0] === 10 && sl[2] === 30;
        ok;
    )";
    TaggedValue r2 = agent.EvaluateScript(script2);
    assert(r2.IsBoolean() && r2.AsBoolean());

    // 3. indexOf & includes & join
    std::string script3 = R"(
        let arr = new Int32Array(4);
        arr[0] = 5; arr[1] = 15; arr[2] = 25; arr[3] = 35;
        let has25 = arr.includes(25);
        let idx15 = arr.indexOf(15);
        let joined = arr.join("-");
        has25 && idx15 === 1 && joined === "5-15-25-35";
    )";
    TaggedValue r3 = agent.EvaluateScript(script3);
    assert(r3.IsBoolean() && r3.AsBoolean());

    std::cout << "  -> fill, set, slice, indexOf, includes, and join verified.\n" << std::flush;
}

void TestGenerationalGCBufferRetention() {
    std::cout << "[Test 6] Verifying Generational GC Retention & Pointer Tracing for TypedArrays...\n" << std::flush;

    Agent agent;
    Heap& heap = agent.GetHeap();
    Realm& realm = agent.GetRealm();

    HandleScope scope(heap);

    // Create TypedArray with backing buffer
    auto* buf = AllocateRuntime<JSArrayBuffer>(64, realm.GetRootShape());
    auto* ta = AllocateRuntime<JSTypedArray>(TypedArrayKind::Uint8, buf, 0, 64, realm.GetRootShape());
    Handle<JSTypedArray> ta_handle = scope.CreateHandle<JSTypedArray>(ta);

    for (size_t i = 0; i < 64; ++i) {
        ta_handle->SetElement(i, TaggedValue::FromSmi(static_cast<int32_t>(i + 5)));
    }

    // Allocate garbage and trigger Minor GC
    for (int i = 0; i < 5000; ++i) {
        heap.Allocate<JSObject>(realm.GetRootShape());
    }
    heap.MinorGC();
    assert(heap.MinorGCCount() > 0);

    // Verify TypedArray and its internal buffer survived GC cycles intact
    assert(ta_handle.Get() != nullptr);
    assert(ta_handle->Buffer() != nullptr);
    assert(ta_handle->Length() == 64);
    for (size_t i = 0; i < 64; ++i) {
        assert(ta_handle->GetElement(i).AsSmi() == static_cast<int32_t>(i + 5));
    }

    // Trigger Major GC
    heap.MajorGC();
    assert(heap.MajorGCCount() > 0);
    for (size_t i = 0; i < 64; ++i) {
        assert(ta_handle->GetElement(i).AsSmi() == static_cast<int32_t>(i + 5));
    }

    std::cout << "  -> TypedArray and backing ArrayBuffer retained cleanly across Minor & Major GC cycles.\n" << std::flush;
}

void TestEndToEndECMAScriptExecution() {
    std::cout << "[Test 7] End-to-End ECMAScript Script Evaluation with TypedArrays & DataView...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // Allocate 16-byte buffer
        let buf = new ArrayBuffer(16);
        let isViewBuf = ArrayBuffer.isView(buf); // false
        let u8 = new Uint8Array(buf);
        let isViewU8 = ArrayBuffer.isView(u8);   // true

        for (let i = 0; i < 16; i++) {
            u8[i] = i * 2;
        }

        let dv = new DataView(buf);
        let isViewDv = ArrayBuffer.isView(dv);   // true

        // Read through DataView
        let b0 = dv.getUint8(0);
        let b1 = dv.getUint8(1);
        let w0 = dv.getUint16(0, false); // Big endian: (0 << 8) | 2 = 2
        let w1 = dv.getUint16(0, true);  // Little endian: (2 << 8) | 0 = 512

        // Float64 view over new buffer
        let f64 = new Float64Array(2);
        f64[0] = 12.5;
        f64[1] = 3.5;
        let sum = f64[0] + f64[1];

        !isViewBuf && isViewU8 && isViewDv && b0 === 0 && b1 === 2 && w0 === 2 && w1 === 512 && sum === 16.0;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean());
    assert(result.AsBoolean());

    std::cout << "  -> End-to-end ECMAScript evaluation with ArrayBuffer, DataView, and TypedArray verified.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone 11: TypedArrays & Binary Buffers\n";
    std::cout << "=========================================================\n\n";

    TestArrayBufferAllocationAndSlicing();
    TestTypedArrayCreationAndIndexing();
    TestArrayBufferSharingAndSubarrays();
    TestDataViewEndianness();
    TestTypedArrayHigherOrderMethods();
    TestGenerationalGCBufferRetention();
    TestEndToEndECMAScriptExecution();

    std::cout << "\n[PASS] All 7 Milestone 11 TypedArray & Buffer Verification Suites Passed!\n";
    return 0;
}
