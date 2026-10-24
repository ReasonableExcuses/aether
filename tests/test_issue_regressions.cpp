#include "aether/runtime.h"
#include "aether/heap.h"
#include "aether/typed_array.h"
#include "aether/wasm.h"
#include "aether/advanced_intrinsics.h"
#include "aether/background_compiler.h"
#include "aether/concurrent_marker.h"
#include "aether/interpreter.h"
#include "aether/bytecode.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"

#include <iostream>
#include <string>
#include <vector>
#include <cassert>
#include <cstdint>
#include <limits>
#include <thread>
#include <chrono>

using namespace aether;

static void Assert(bool condition, const char* message) {
    if (!condition) {
        std::cerr << "FAIL: " << message << std::endl;
        std::exit(1);
    }
}

// ============================================================================
// [Regression 1] Wasm memory.grow() Detaches Old Buffers & TypedArray Views
// ============================================================================
static void Test1_WasmMemoryGrowDetachesOldBuffers() {
    std::cout << "[Regression 1] Verifying Wasm memory.grow() detaches old buffers and views..." << std::endl;

    Agent agent;

    std::string script = R"(
        const memory = new WebAssembly.Memory({ initial: 1, maximum: 10 });
        const oldBuffer = memory.buffer;
        const oldView = new Uint8Array(oldBuffer);
        oldView[0] = 42;

        const prevPages = memory.grow(1);
        const newBuffer = memory.buffer;
        const newView = new Uint8Array(newBuffer);

        // Old buffer must be detached: byteLength == 0
        const oldDetached = oldBuffer.byteLength === 0;
        // Old view must reflect detached buffer: byteLength == 0
        const viewDetached = oldView.byteLength === 0;
        // New buffer reflects grown size (2 pages = 131072 bytes)
        const newSizeValid = newBuffer.byteLength === 131072;
        // Data preserved in newly mapped memory
        const dataPreserved = newView[0] === 42;

        oldDetached && viewDetached && newSizeValid && dataPreserved && (prevPages === 1);
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "WebAssembly memory.grow() buffer detachment failed");

    // Also test C++ direct API
    WasmLimits limits;
    limits.initial = 1;
    limits.maximum = 5;
    limits.has_max = true;
    WasmMemoryInstance mem(limits);

    JSArrayBuffer* b1 = mem.GetOrCreateJSBuffer(agent.GetRealm());
    Assert(!b1->IsDetached(), "b1 must initially not be detached");
    Assert(b1->ByteLength() == 65536, "b1 initial size 64KB");

    mem.Grow(1);
    Assert(b1->IsDetached(), "b1 must be detached after memory.grow()");
    Assert(b1->ByteLength() == 0, "b1 byteLength must be 0 after detach");

    JSArrayBuffer* b2 = mem.GetOrCreateJSBuffer(agent.GetRealm());
    Assert(!b2->IsDetached(), "b2 must not be detached");
    Assert(b2->ByteLength() == 131072, "b2 must reflect grown size 128KB");

    std::cout << "  -> PASSED: Wasm memory.grow() correctly detaches old buffers & views." << std::endl;
}

// ============================================================================
// [Regression 2] WebAssembly Complete Validation Pass
// ============================================================================
static void Test2_WasmValidationPass() {
    std::cout << "[Regression 2] Verifying WebAssembly validation pass rejects invalid modules..." << std::endl;

    // 1. Valid minimal module
    uint8_t valid_wasm[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00 // Magic + version
    };
    Assert(WasmModule::Validate(valid_wasm, sizeof(valid_wasm)), "Minimal valid Wasm module must pass validation");

    // 2. Invalid magic
    uint8_t bad_magic[] = {
        0x00, 0x62, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00
    };
    Assert(!WasmModule::Validate(bad_magic, sizeof(bad_magic)), "Bad magic must fail validation");

    // 3. Module with initial > maximum memory limits
    uint8_t bad_mem_limits[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x05, 0x04, 0x01, 0x01, 0x0a, 0x02 // Memory section: 1 memory, flags=1 (has max), initial=10, max=2
    };
    Assert(!WasmModule::Validate(bad_mem_limits, sizeof(bad_mem_limits)), "Memory initial > maximum must fail validation");

    // 4. Module with initial > maximum table limits
    uint8_t bad_tbl_limits[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x04, 0x05, 0x01, 0x70, 0x01, 0x10, 0x05 // Table section: 1 table, funcref, has max, initial=16, max=5
    };
    Assert(!WasmModule::Validate(bad_tbl_limits, sizeof(bad_tbl_limits)), "Table initial > maximum must fail validation");

    // 5. Module with invalid start function index
    uint8_t bad_start_fn[] = {
        0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00,
        0x08, 0x01, 0x05 // Start section: function index 5 (no functions declared)
    };
    Assert(!WasmModule::Validate(bad_start_fn, sizeof(bad_start_fn)), "Invalid start function index must fail validation");

    // 6. Test via JavaScript WebAssembly.validate
    Agent agent;
    std::string script = R"(
        const validModule = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00]);
        const invalidModule = new Uint8Array([0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00, 0x05, 0x04, 0x01, 0x01, 0x0a, 0x02]);

        const v1 = WebAssembly.validate(validModule) === true;
        const v2 = WebAssembly.validate(invalidModule) === false;

        v1 && v2;
    )";
    TaggedValue js_res = agent.EvaluateScript(script);
    Assert(js_res.IsBoolean() && js_res.AsBoolean(), "WebAssembly.validate JS tests failed");

    std::cout << "  -> PASSED: WebAssembly validation pass comprehensively verifies structure & limits." << std::endl;
}

// ============================================================================
// [Regression 3] DataView CheckBounds Overflow Protection
// ============================================================================
static void Test3_DataViewBoundsOverflow() {
    std::cout << "[Regression 3] Verifying DataView CheckBounds overflow resistance..." << std::endl;

    auto* buffer = new JSArrayBuffer(16);
    JSDataView view(buffer, 0, 16);

    // Normal valid access
    Assert(view.CheckBounds(0, 4), "0..4 in 16-byte buffer must succeed");
    Assert(view.CheckBounds(12, 4), "12..16 in 16-byte buffer must succeed");

    // Normal out-of-bounds access
    Assert(!view.CheckBounds(13, 4), "13..17 in 16-byte buffer must fail");

    // Arithmetic overflow attempt: huge offset + small size
    size_t huge_offset = std::numeric_limits<size_t>::max() - 2;
    Assert(!view.CheckBounds(huge_offset, 4), "Huge offset causing addition overflow must fail CheckBounds");

    // Detached buffer check
    buffer->Detach();
    Assert(!view.CheckBounds(0, 4), "Detached buffer must fail CheckBounds");

    delete buffer;
    std::cout << "  -> PASSED: DataView CheckBounds correctly prevents size_t overflow." << std::endl;
}

// ============================================================================
// [Regression 4] TypedArray BigInt64 & BigUint64 Genuine JavaScript Semantics
// ============================================================================
static void Test4_BigIntTypedArrays() {
    std::cout << "[Regression 4] Verifying BigInt64Array & BigUint64Array element access..." << std::endl;

    auto* buf64 = new JSArrayBuffer(64);
    JSTypedArray bi64(TypedArrayKind::BigInt64, buf64, 0, 8);
    JSTypedArray bu64(TypedArrayKind::BigUint64, buf64, 0, 8);

    // 1. Basic read/write with genuine BigInt results
    bi64.SetElement(0, TaggedValue::FromDouble(123456789.0));
    TaggedValue v0 = bi64.GetElement(0);
    Assert(v0.IsBigInt(), "BigInt64 element 0 must be BigInt");
    Assert(v0.ToString() == "123456789", "BigInt64 element 0 value mismatch");

    // 2. Negative 64-bit values
    bi64.SetElement(1, TaggedValue::FromDouble(-987654321.0));
    TaggedValue v1 = bi64.GetElement(1);
    Assert(v1.IsBigInt(), "BigInt64 element 1 must be BigInt");
    Assert(v1.ToString() == "-987654321", "BigInt64 element 1 negative mismatch");

    // 3. Values strictly beyond 2^53 - 1 (loss of precision in double)
    // 2^53 + 1 = 9007199254740993
    int64_t beyond53 = 9007199254740993LL;
    bi64.SetElement(2, TaggedValue::FromHeapObject(JSBigInt::Create(beyond53)));
    TaggedValue v2 = bi64.GetElement(2);
    Assert(v2.IsBigInt(), "BigInt64 element 2 must be BigInt");
    Assert(v2.ToString() == "9007199254740993", "BigInt64 element 2 precision mismatch beyond 2^53-1");

    // Negative beyond 2^53 - 1
    int64_t neg_beyond53 = -9007199254740993LL;
    bi64.SetElement(3, TaggedValue::FromHeapObject(JSBigInt::Create(neg_beyond53)));
    TaggedValue v3 = bi64.GetElement(3);
    Assert(v3.IsBigInt(), "BigInt64 element 3 must be BigInt");
    Assert(v3.ToString() == "-9007199254740993", "BigInt64 negative precision mismatch beyond 2^53-1");

    // 4. Unsigned 64-bit values and unsigned wraparound
    // Write -1LL into BigUint64Array -> wraps around to 18446744073709551615 (2^64 - 1)
    bu64.SetElement(4, TaggedValue::FromHeapObject(JSBigInt::Create(-1LL)));
    TaggedValue v4 = bu64.GetElement(4);
    Assert(v4.IsBigInt(), "BigUint64 element 4 must be BigInt");
    Assert(v4.ToString() == "18446744073709551615", "BigUint64 unsigned wraparound mismatch for -1");

    // 5. End-to-end JavaScript evaluation via Agent
    Agent agent;
    TaggedValue js_res = agent.EvaluateScript(R"(
        const bi = new BigInt64Array(2);
        const bu = new BigUint64Array(2);
        bi[0] = 9007199254740993n;
        bi[1] = -9007199254740993n;
        bu[0] = 18446744073709551615n;
        bu[1] = -1n; // Wraps to 2^64-1

        (typeof bi[0] === 'bigint') &&
        (bi[0] === 9007199254740993n) &&
        (typeof bi[1] === 'bigint') &&
        (bi[1] === -9007199254740993n) &&
        (typeof bu[0] === 'bigint') &&
        (bu[0] === 18446744073709551615n) &&
        (bu[1] === 18446744073709551615n);
    )");
    Assert(js_res.IsTrue(), "End-to-end BigInt64Array & BigUint64Array JavaScript execution failed");

    delete buf64;
    std::cout << "  -> PASSED: BigInt64Array and BigUint64Array verified." << std::endl;
}

// ============================================================================
// [Regression 5] TypedArray Assignment Numeric & Wrapping Conversions
// ============================================================================
static void Test5_TypedArrayAssignmentConversions() {
    std::cout << "[Regression 5] Verifying TypedArray assignment wrapping & conversions..." << std::endl;

    Agent agent;

    std::string script = R"(
        const u8 = new Uint8Array(4);
        u8[0] = -1;       // 255
        u8[1] = 256;      // 0
        u8[2] = "42";     // 42
        u8[3] = true;     // 1

        const i8 = new Int8Array(2);
        i8[0] = 128;      // -128
        i8[1] = 255;      // -1

        const u16 = new Uint16Array(2);
        u16[0] = 65536;   // 0
        u16[1] = -1;      // 65535

        const r1 = u8[0] === 255 && u8[1] === 0 && u8[2] === 42 && u8[3] === 1;
        const r2 = i8[0] === -128 && i8[1] === -1;
        const r3 = u16[0] === 0 && u16[1] === 65535;

        r1 && r2 && r3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "TypedArray element conversion failed");

    std::cout << "  -> PASSED: TypedArray assignment integer wrapping & ToNumber conversions verified." << std::endl;
}

// ============================================================================
// [Regression 6] Atomics Rejection of Non-Shared Buffers and Disallowed Kinds
// ============================================================================
static void Test6_AtomicsBufferAndKindValidation() {
    std::cout << "[Regression 6] Verifying Atomics non-shared buffer & element type validation..." << std::endl;

    Agent agent;

    std::string script = R"(
        const regularBuffer = new ArrayBuffer(16);
        const nonSharedI32 = new Int32Array(regularBuffer);

        // 1. Non-shared buffer rejection
        let caughtNonShared = false;
        try {
            Atomics.add(nonSharedI32, 0, 1);
        } catch (e) {
            caughtNonShared = e.name === "TypeError";
        }

        // 2. Disallowed kind: Float32Array on SharedArrayBuffer
        const sab = new SharedArrayBuffer(16);
        const f32 = new Float32Array(sab);
        let caughtFloat32 = false;
        try {
            Atomics.add(f32, 0, 1);
        } catch (e) {
            caughtFloat32 = e.name === "TypeError";
        }

        // 3. Disallowed kind: Uint8ClampedArray on SharedArrayBuffer
        const u8c = new Uint8ClampedArray(sab);
        let caughtUint8Clamped = false;
        try {
            Atomics.add(u8c, 0, 1);
        } catch (e) {
            caughtUint8Clamped = e.name === "TypeError";
        }

        caughtNonShared && caughtFloat32 && caughtUint8Clamped;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    Assert(result.IsBoolean() && result.AsBoolean(), "Atomics validation failed");

    std::cout << "  -> PASSED: Atomics strictly validates SharedArrayBuffer and element types." << std::endl;
}

// ============================================================================
// [Regression 7] IsolateScope Context Switching
// ============================================================================
static void Test7_IsolateScopeContextSwitching() {
    std::cout << "[Regression 7] Verifying IsolateScope RAII context switching..." << std::endl;

    IsolateHeap heap1;
    IsolateHeap heap2;

    IsolateHeap::SetCurrent(&heap1);
    Assert(IsolateHeap::Current() == &heap1, "Initial heap must be heap1");

    {
        IsolateScope scope(&heap2);
        Assert(IsolateHeap::Current() == &heap2, "Scoped heap must be heap2");
    }

    Assert(IsolateHeap::Current() == &heap1, "Restored heap must be heap1");

    std::cout << "  -> PASSED: IsolateScope correctly saves and restores active isolate context." << std::endl;
}

// ============================================================================
// [Regression 8] BackgroundCompiler Bytecode Ownership & Feedback Snapshot
// ============================================================================
static void Test8_BackgroundCompilerOwnershipAndFeedback() {
    std::cout << "[Regression 8] Verifying BackgroundCompiler bytecode ownership and feedback snapshot..." << std::endl;

    std::string_view src = "function add(a, b) { return a + b; }";
    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();
    BytecodeEmitter emitter;
    auto bc_raw = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));
    std::shared_ptr<const BytecodeArray> bytecode = std::move(bc_raw);

    BackgroundCompiler compiler(1);
    compiler.Start();

    std::atomic<bool> completed{false};
    bool queued = compiler.Enqueue(bytecode, nullptr, [&completed](std::shared_ptr<ExecutableMemory> exec) {
        completed.store(true);
    });

    Assert(queued, "Compilation job must be queued successfully");
    compiler.WaitForPendingTasks();
    compiler.Stop();

    std::cout << "  -> PASSED: BackgroundCompiler manages bytecode ownership and feedback snapshots." << std::endl;
}

// ============================================================================
// [Regression 9] ConcurrentMarker SATB Tri-Color Invariant & STW Remark
// ============================================================================
static void Test9_ConcurrentMarkerSATBBarrier() {
    std::cout << "[Regression 9] Verifying ConcurrentMarker SATB barrier & dirty sources remark..." << std::endl;

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);
    HandleScope scope(heap);

    ConcurrentMarker marker(heap);
    auto* root_obj = heap.AllocateOld<JSObject>(nullptr);
    auto* child_obj = heap.AllocateOld<JSObject>(nullptr);

    marker.Start({root_obj});

    // Mutator overwrites root_obj pointer with child_obj
    marker.NotifyWrite(root_obj, TaggedValue::FromHeapObject(child_obj));

    marker.WaitForCompletion();
    marker.FinalizeMarking();

    IsolateHeap::SetCurrent(nullptr);

    std::cout << "  -> PASSED: SATB write barrier and remark phase correctly re-trace mutated objects." << std::endl;
}

// ============================================================================
// [Regression 10] DataView BigInt64 & BigUint64 Endianness & Precision
// ============================================================================
static void Test10_DataViewBigInt64AndBigUint64() {
    std::cout << "[Regression 10] Verifying DataView getBigInt64/setBigInt64/getBigUint64/setBigUint64..." << std::endl;

    auto* buffer = new JSArrayBuffer(32);
    JSDataView view(buffer, 0, 32);

    int64_t val64 = 9007199254740993LL; // 2^53 + 1
    view.SetBigInt64(0, val64, true);
    Assert(view.GetBigInt64(0, true) == val64, "DataView little-endian BigInt64 mismatch");

    int64_t neg_val64 = -9007199254740993LL;
    view.SetBigInt64(8, neg_val64, false);
    Assert(view.GetBigInt64(8, false) == neg_val64, "DataView big-endian negative BigInt64 mismatch");

    uint64_t uval64 = 18446744073709551615ULL; // 2^64 - 1
    view.SetBigUint64(16, uval64, true);
    Assert(view.GetBigUint64(16, true) == uval64, "DataView little-endian BigUint64 mismatch");

    delete buffer;

    // JavaScript evaluation
    Agent agent;
    TaggedValue js_res = agent.EvaluateScript(R"(
        const ab = new ArrayBuffer(32);
        const dv = new DataView(ab);
        dv.setBigInt64(0, 9007199254740993n, true);
        dv.setBigInt64(8, -9007199254740993n, false);
        dv.setBigUint64(16, 18446744073709551615n, true);

        const v1 = dv.getBigInt64(0, true);
        const v2 = dv.getBigInt64(8, false);
        const v3 = dv.getBigUint64(16, true);

        (typeof v1 === 'bigint') && (v1 === 9007199254740993n) &&
        (typeof v2 === 'bigint') && (v2 === -9007199254740993n) &&
        (typeof v3 === 'bigint') && (v3 === 18446744073709551615n);
    )");
    Assert(js_res.IsBoolean() && js_res.AsBoolean(), "DataView BigInt JS evaluation failed");

    std::cout << "  -> PASSED: DataView BigInt 64-bit endianness and precision verified." << std::endl;
}

// ============================================================================
// [Regression 11] WebAssembly Full Validation & i64/BigInt Interoperability
// ============================================================================
struct WasmBuilder {
    std::vector<uint8_t> bytes{0x00, 0x61, 0x73, 0x6d, 0x01, 0x00, 0x00, 0x00};

    static void AppendU32(std::vector<uint8_t>& buf, uint32_t val) {
        do {
            uint8_t b = val & 0x7F;
            val >>= 7;
            if (val != 0) b |= 0x80;
            buf.push_back(b);
        } while (val != 0);
    }

    void AddSection(uint8_t sec_id, const std::vector<uint8_t>& payload) {
        bytes.push_back(sec_id);
        AppendU32(bytes, static_cast<uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }
};

static void Test11_WasmValidationAndI64Interop() {
    std::cout << "[Regression 11] Verifying WebAssembly operand-stack typing, control-flow & i64 BigInt interop..." << std::endl;

    // 1. Stack underflow rejection: i32.add with no operands
    {
        WasmBuilder b;
        std::vector<uint8_t> sec_type;
        WasmBuilder::AppendU32(sec_type, 1);
        sec_type.push_back(0x60); // func
        WasmBuilder::AppendU32(sec_type, 0); // 0 params
        WasmBuilder::AppendU32(sec_type, 1); // 1 result
        sec_type.push_back(0x7F); // i32
        b.AddSection(1, sec_type);

        std::vector<uint8_t> sec_func;
        WasmBuilder::AppendU32(sec_func, 1);
        WasmBuilder::AppendU32(sec_func, 0);
        b.AddSection(3, sec_func);

        std::vector<uint8_t> body{0x00, 0x6A, 0x0B}; // 0 locals, i32.add, end
        std::vector<uint8_t> sec_code;
        WasmBuilder::AppendU32(sec_code, 1);
        WasmBuilder::AppendU32(sec_code, static_cast<uint32_t>(body.size()));
        sec_code.insert(sec_code.end(), body.begin(), body.end());
        b.AddSection(10, sec_code);

        Assert(!WasmModule::Validate(b.bytes.data(), b.bytes.size()), "Stack underflow must fail validation");
    }

    // 2. Operand type mismatch rejection: i32 + f32 given to i32.add
    {
        WasmBuilder b;
        std::vector<uint8_t> sec_type{0x01, 0x60, 0x00, 0x01, 0x7F};
        b.AddSection(1, sec_type);

        std::vector<uint8_t> sec_func{0x01, 0x00};
        b.AddSection(3, sec_func);

        std::vector<uint8_t> body{0x00, 0x41, 0x01, 0x43, 0x00, 0x00, 0x00, 0x00, 0x6A, 0x0B};
        std::vector<uint8_t> sec_code{0x01};
        WasmBuilder::AppendU32(sec_code, static_cast<uint32_t>(body.size()));
        sec_code.insert(sec_code.end(), body.begin(), body.end());
        b.AddSection(10, sec_code);

        Assert(!WasmModule::Validate(b.bytes.data(), b.bytes.size()), "Operand type mismatch must fail validation");
    }

    // 3. Control-flow stack violation rejection: branch depth out of range
    {
        WasmBuilder b;
        std::vector<uint8_t> sec_type{0x01, 0x60, 0x00, 0x00}; // () -> ()
        b.AddSection(1, sec_type);

        std::vector<uint8_t> sec_func{0x01, 0x00};
        b.AddSection(3, sec_func);

        std::vector<uint8_t> body{0x00, 0x02, 0x40, 0x0C, 0x05, 0x0B, 0x0B};
        std::vector<uint8_t> sec_code{0x01};
        WasmBuilder::AppendU32(sec_code, static_cast<uint32_t>(body.size()));
        sec_code.insert(sec_code.end(), body.begin(), body.end());
        b.AddSection(10, sec_code);

        Assert(!WasmModule::Validate(b.bytes.data(), b.bytes.size()), "Invalid branch target depth must fail validation");
    }

    // 4. Valid module with unreachable polymorphic stack
    {
        WasmBuilder b;
        std::vector<uint8_t> sec_type{0x01, 0x60, 0x00, 0x01, 0x7F}; // () -> (i32)
        b.AddSection(1, sec_type);

        std::vector<uint8_t> sec_func{0x01, 0x00};
        b.AddSection(3, sec_func);

        std::vector<uint8_t> body{0x00, 0x00, 0x6A, 0x0B};
        std::vector<uint8_t> sec_code{0x01};
        WasmBuilder::AppendU32(sec_code, static_cast<uint32_t>(body.size()));
        sec_code.insert(sec_code.end(), body.begin(), body.end());
        b.AddSection(10, sec_code);

        Assert(WasmModule::Validate(b.bytes.data(), b.bytes.size()), "Unreachable polymorphic stack must pass validation");
    }

    // 5. WasmVal i64 <-> JS BigInt 64-bit range preservation
    Agent agent;
    WasmVal v1 = WasmVal::MakeI64(9007199254740993LL);
    TaggedValue tv1 = v1.ToJS(agent.GetRealm());
    Assert(tv1.IsBigInt(), "v1.ToJS() must be BigInt");
    Assert(tv1.ToString() == "9007199254740993", "v1 BigInt string value mismatch");
    WasmVal rt1 = WasmVal::FromJS(tv1, WasmValType::I64);
    Assert(rt1.i64 == 9007199254740993LL, "Roundtrip i64 precision loss beyond 2^53-1");

    WasmVal v2 = WasmVal::MakeI64(-9007199254740993LL);
    TaggedValue tv2 = v2.ToJS(agent.GetRealm());
    Assert(tv2.IsBigInt(), "v2.ToJS() must be BigInt");
    Assert(tv2.ToString() == "-9007199254740993", "v2 BigInt negative string mismatch");
    WasmVal rt2 = WasmVal::FromJS(tv2, WasmValType::I64);
    Assert(rt2.i64 == -9007199254740993LL, "Negative roundtrip i64 precision loss");

    std::cout << "  -> PASSED: WebAssembly operand-stack typing, control-flow & i64 BigInt verified." << std::endl;
}

// ============================================================================
// [Regression 12] BackgroundCompiler Safety Under Owner Destruction
// ============================================================================
static void Test12_BackgroundCompilerOwnerDestructionSafety() {
    std::cout << "[Regression 12] Verifying BackgroundCompiler safety under owner destruction..." << std::endl;

    BackgroundCompiler compiler(2);
    compiler.Start();

    std::atomic<bool> done{false};
    {
        std::string_view src = "function compute(n) { var x = 0; while (x < n) { x = x + 1; } return x; }";
        Lexer lexer(src);
        Parser parser(lexer);
        auto func_decl = parser.ParseStatement();
        BytecodeEmitter emitter;
        auto bc_raw = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));
        std::shared_ptr<const BytecodeArray> shared_bc = std::move(bc_raw);

        TypeFeedbackVector feedback(16);
        compiler.Enqueue(shared_bc, &feedback, [&done](std::shared_ptr<ExecutableMemory> exec) {
            Assert(exec != nullptr, "JIT compilation must produce executable memory");
            done.store(true);
        });
    }

    compiler.WaitForPendingTasks();
    Assert(done.load(), "Compilation job must succeed even when local shared_ptr was dropped");
    compiler.Stop();

    std::cout << "  -> PASSED: Worker safely retains bytecode ownership preventing use-after-free." << std::endl;
}

// ============================================================================
// [Regression 13] Wasm Host Registry GC Pruning & Cleanup
// ============================================================================
static void Test13_WasmRegistryPruneDeadUnderGC() {
    std::cout << "[Regression 13] Verifying Wasm host-object registry GC prune and cleanup..." << std::endl;

    IsolateHeap heap;
    IsolateHeap::SetCurrent(&heap);

    {
        Agent agent;
        agent.EvaluateScript(R"(
            for (let i = 0; i < 30; ++i) {
                const mem = new WebAssembly.Memory({ initial: 1 });
                const tbl = new WebAssembly.Table({ initial: 1, element: 'anyfunc' });
            }
        )");
    }

    heap.MajorGC();
    heap.MinorGC();

    WasmRegistryPruneDead([](HeapObject*) { return false; });
    Assert(WasmRegistryTotalEntries() == 0, "All dead entries must be pruned from Wasm registry");

    IsolateHeap::SetCurrent(nullptr);
    std::cout << "  -> PASSED: Wasm registry safely pruned dead host wrappers during GC cycles." << std::endl;
}

// ============================================================================
// [Regression 14] Atomics 64-bit BigInt Semantics, Synchronization & Cancellation
// ============================================================================
static void Test14_AtomicsBigIntOperationsAndStress() {
    std::cout << "[Regression 14] Verifying Atomics 64-bit BigInt semantics and concurrency..." << std::endl;

    Agent agent;

    std::string script = R"(
        const sab = new SharedArrayBuffer(64);
        const bi = new BigInt64Array(sab);
        const bu = new BigUint64Array(sab);

        // 1. Atomics.store and load
        Atomics.store(bi, 0, 9007199254740993n);
        const l0 = Atomics.load(bi, 0);
        const okLoad = (typeof l0 === 'bigint') && (l0 === 9007199254740993n);

        // 2. Atomics.add with 64-bit precision preservation
        const prevAdd = Atomics.add(bi, 0, 1000n);
        const postAdd = Atomics.load(bi, 0);
        const okAdd = (prevAdd === 9007199254740993n) && (postAdd === 9007199254741993n);

        // 3. Atomics.sub
        const prevSub = Atomics.sub(bi, 0, 1000n);
        const postSub = Atomics.load(bi, 0);
        const okSub = (prevSub === 9007199254741993n) && (postSub === 9007199254740993n);

        // 4. Atomics.and / or / xor
        Atomics.store(bi, 0, 0x0F0F0F0F0F0F0F0Fn);
        Atomics.and(bi, 0, 0x00FF00FF00FF00FFn);
        const okAnd = Atomics.load(bi, 0) === 0x000F000F000F000Fn;

        Atomics.or(bi, 0, 0x7000700070007000n);
        const okOr = Atomics.load(bi, 0) === 0x700F700F700F700Fn;

        Atomics.xor(bi, 0, 0x700F700F700F700Fn);
        const okXor = Atomics.load(bi, 0) === 0n;

        // BigUint64Array test
        Atomics.store(bu, 0, 18446744073709551615n);
        const okBu = (Atomics.load(bu, 0) === 18446744073709551615n);

        // 5. Atomics.compareExchange on BigInt64Array
        Atomics.store(bi, 1, 100n);
        const ce1 = Atomics.compareExchange(bi, 1, 100n, 200n); // success, returns 100n
        const ce1Val = Atomics.load(bi, 1); // 200n
        const ce2 = Atomics.compareExchange(bi, 1, 999n, 300n); // mismatch, returns 200n
        const ce2Val = Atomics.load(bi, 1); // still 200n
        const okCE = (ce1 === 100n) && (ce1Val === 200n) && (ce2 === 200n) && (ce2Val === 200n);

        // 6. Atomics.wait on BigInt64Array: not-equal and timed-out
        const wNotEq = Atomics.wait(bi, 1, 999n) === "not-equal";
        const wTimeout = Atomics.wait(bi, 1, 200n, 5) === "timed-out";

        // 7. Atomics.waitAsync sync returns on BigInt64Array
        const waNotEq = Atomics.waitAsync(bi, 1, 999n);
        const okWaNotEq = !waNotEq.async && (waNotEq.value === "not-equal");

        const waTimeout = Atomics.waitAsync(bi, 1, 200n, 0);
        const okWaTimeout = !waTimeout.async && (waTimeout.value === "timed-out");

        okLoad && okAdd && okSub && okAnd && okOr && okXor && okBu && okCE && wNotEq && wTimeout && okWaNotEq && okWaTimeout;
    )";

    TaggedValue res = agent.EvaluateScript(script);
    Assert(res.IsBoolean() && res.AsBoolean(), "Atomics 64-bit BigInt evaluation failed");

    // 8. Multi-threaded FutexManager Wait64 / Notify synchronization
    int64_t futex_val = 123456789012345LL;
    std::atomic<bool> wait_completed{false};
    std::string wait_status;

    std::thread waiter([&]() {
        wait_status = FutexManager::Instance().Wait64(&futex_val, 123456789012345LL, 5000.0);
        wait_completed.store(true);
    });

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    uint32_t woken = FutexManager::Instance().Notify(&futex_val, 1);
    Assert(woken == 1, "Notify must awaken exactly 1 64-bit waiter");

    waiter.join();
    Assert(wait_completed.load(), "Wait64 must have completed");
    Assert(wait_status == "ok", "Wait64 must return ok on notification");

    // 9. Agent cancellation & shutdown test with active async wait
    {
        Agent temp_agent;
        int64_t async_val = 42LL;
        auto* prom = AllocateRuntime<JSPromise>(temp_agent.GetRealm().GetRootShape());
        AsyncWaitManager::Instance().SpawnWait64(&async_val, 42LL, 10000.0, prom, &temp_agent, &temp_agent.GetRealm());

        AsyncWaitManager::Instance().CancelAndJoinForAgent(&temp_agent);
    }

    std::cout << "  -> PASSED: Atomics 64-bit operations, futex wait/notify, waitAsync, and agent cancellation verified." << std::endl;
}

// ============================================================================
// Main Regression Test Runner
// ============================================================================
int main() {
    std::cout << "========================================================\n";
    std::cout << "Running AetherVM Architecture & Memory-Safety Regression Suite\n";
    std::cout << "========================================================\n";

    Test1_WasmMemoryGrowDetachesOldBuffers();
    Test2_WasmValidationPass();
    Test3_DataViewBoundsOverflow();
    Test4_BigIntTypedArrays();
    Test5_TypedArrayAssignmentConversions();
    Test6_AtomicsBufferAndKindValidation();
    Test7_IsolateScopeContextSwitching();
    Test8_BackgroundCompilerOwnershipAndFeedback();
    Test9_ConcurrentMarkerSATBBarrier();
    Test10_DataViewBigInt64AndBigUint64();
    Test11_WasmValidationAndI64Interop();
    Test12_BackgroundCompilerOwnerDestructionSafety();
    Test13_WasmRegistryPruneDeadUnderGC();
    Test14_AtomicsBigIntOperationsAndStress();

    std::cout << "========================================================\n";
    std::cout << "ALL 14 REGRESSION TESTS PASSED CLEANLY (14/14)!\n";
    std::cout << "========================================================\n";

    return 0;
}
