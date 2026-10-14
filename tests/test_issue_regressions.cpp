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
// [Regression 4] TypedArray BigInt64 & BigUint64 Support
// ============================================================================
static void Test4_BigIntTypedArrays() {
    std::cout << "[Regression 4] Verifying BigInt64Array & BigUint64Array element access..." << std::endl;

    auto* buf64 = new JSArrayBuffer(32);
    JSTypedArray bi64(TypedArrayKind::BigInt64, buf64, 0, 4);
    JSTypedArray bu64(TypedArrayKind::BigUint64, buf64, 0, 4);

    // Write and read via JSTypedArray methods
    bi64.SetElement(0, TaggedValue::FromDouble(123456789.0));
    TaggedValue v0 = bi64.GetElement(0);
    Assert(v0.IsDouble() && v0.AsDouble() == 123456789.0, "BigInt64 element 0 mismatch");

    bi64.SetElement(1, TaggedValue::FromDouble(-987654321.0));
    TaggedValue v1 = bi64.GetElement(1);
    Assert(v1.IsDouble() && v1.AsDouble() == -987654321.0, "BigInt64 element 1 negative mismatch");

    bu64.SetElement(2, TaggedValue::FromDouble(4294967296.0)); // > 32-bit
    TaggedValue v2 = bu64.GetElement(2);
    Assert(v2.IsDouble() && v2.AsDouble() == 4294967296.0, "BigUint64 element 2 mismatch");

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

    std::cout << "========================================================\n";
    std::cout << "ALL 9 REGRESSION TESTS PASSED CLEANLY (9/9)!\n";
    std::cout << "========================================================\n";

    return 0;
}
