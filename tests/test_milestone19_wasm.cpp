#include "aether/wasm.h"
#include "aether/runtime.h"
#include "aether/interpreter.h"
#include "aether/typed_array.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <cstring>

using namespace aether;

// ============================================================================
// WasmBinaryBuilder Utility for Handcrafting Conforming Wasm Binaries
// ============================================================================

struct WasmBinaryBuilder {
    std::vector<uint8_t> bytes;

    WasmBinaryBuilder() {
        // Magic "\0asm"
        bytes.push_back(0x00);
        bytes.push_back(0x61);
        bytes.push_back(0x73);
        bytes.push_back(0x6D);
        // Version 1
        bytes.push_back(0x01);
        bytes.push_back(0x00);
        bytes.push_back(0x00);
        bytes.push_back(0x00);
    }

    static void AppendU32(std::vector<uint8_t>& buf, uint32_t val) {
        do {
            uint8_t b = val & 0x7F;
            val >>= 7;
            if (val != 0) b |= 0x80;
            buf.push_back(b);
        } while (val != 0);
    }

    static void AppendI32(std::vector<uint8_t>& buf, int32_t val) {
        bool more = true;
        while (more) {
            uint8_t b = val & 0x7F;
            val >>= 7;
            if ((val == 0 && (b & 0x40) == 0) || (val == -1 && (b & 0x40) != 0)) {
                more = false;
            } else {
                b |= 0x80;
            }
            buf.push_back(b);
        }
    }

    static void AppendString(std::vector<uint8_t>& buf, std::string_view str) {
        AppendU32(buf, static_cast<uint32_t>(str.size()));
        buf.insert(buf.end(), str.begin(), str.end());
    }

    void AddSection(uint8_t sec_id, const std::vector<uint8_t>& payload) {
        bytes.push_back(sec_id);
        AppendU32(bytes, static_cast<uint32_t>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());
    }

    JSArrayBuffer* ToArrayBuffer(Realm& realm) const {
        auto* ab = AllocateRuntime<JSArrayBuffer>(bytes.size(), realm.GetRootShape());
        if (!bytes.empty()) {
            std::memcpy(ab->Data(), bytes.data(), bytes.size());
        }
        return ab;
    }
};

// ============================================================================
// Test 1: WebAssembly Validation, Compilation & Magic/Version Verification
// ============================================================================
void Test1_ValidationAndCompilation() {
    std::cout << "[Test 1] WebAssembly.validate, Compilation and Error Handling..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    // 1. Validate empty/truncated bytes
    uint8_t bad_bytes[] = {0x00, 0x61, 0x00};
    assert(!WasmModule::Validate(bad_bytes, sizeof(bad_bytes)));

    // 2. Validate invalid magic
    uint8_t bad_magic[] = {0x00, 0x62, 0x61, 0x64, 0x01, 0x00, 0x00, 0x00};
    assert(!WasmModule::Validate(bad_magic, sizeof(bad_magic)));

    // 3. Validate valid minimal module
    WasmBinaryBuilder minimal_builder;
    assert(WasmModule::Validate(minimal_builder.bytes.data(), minimal_builder.bytes.size()));

    // 4. Test via JS WebAssembly object
    TaggedValue wasm_val = realm.GetGlobalProperty("WebAssembly");
    assert(wasm_val.IsHeapObject());
    auto* wasm_obj = static_cast<JSObject*>(wasm_val.AsHeapObject());

    auto* valid_ab = minimal_builder.ToArrayBuffer(realm);
    auto* validate_fn = static_cast<JSNativeFunction*>(wasm_obj->GetNamedProperty("validate").AsHeapObject());
    TaggedValue val_res = validate_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(valid_ab)});
    assert(val_res.IsBoolean() && val_res.ToBoolean() == true);

    // 5. Test compile error on bad bytes
    auto* bad_ab = AllocateRuntime<JSArrayBuffer>(sizeof(bad_magic), realm.GetRootShape());
    std::memcpy(bad_ab->Data(), bad_magic, sizeof(bad_magic));
    TaggedValue bad_res = validate_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(bad_ab)});
    assert(bad_res.IsBoolean() && bad_res.ToBoolean() == false);

    std::cout << "  -> PASSED: Valid and invalid modules correctly recognized and validated." << std::endl;
}

// ============================================================================
// Test 2: Basic Numeric Arithmetic Execution (add(i32, i32) -> i32)
// ============================================================================
void Test2_BasicAddFunction() {
    std::cout << "[Test 2] Basic Function Execution (add(i32, i32) -> i32)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type Section (ID 1): (i32, i32) -> (i32)
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 1); // 1 type
    sec_type.push_back(0x60); // form func
    WasmBinaryBuilder::AppendU32(sec_type, 2); // 2 params
    sec_type.push_back(0x7F); // i32
    sec_type.push_back(0x7F); // i32
    WasmBinaryBuilder::AppendU32(sec_type, 1); // 1 result
    sec_type.push_back(0x7F); // i32
    builder.AddSection(1, sec_type);

    // Function Section (ID 3): 1 func, type 0
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    builder.AddSection(3, sec_func);

    // Export Section (ID 7): export "add" func 0
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    WasmBinaryBuilder::AppendString(sec_export, "add");
    sec_export.push_back(0x00); // func kind
    WasmBinaryBuilder::AppendU32(sec_export, 0); // func index 0
    builder.AddSection(7, sec_export);

    // Code Section (ID 10): 1 body
    // local.get 0 (0x20 0x00), local.get 1 (0x20 0x01), i32.add (0x6A), end (0x0B)
    std::vector<uint8_t> body_code = {
        0x20, 0x00,
        0x20, 0x01,
        0x6A,
        0x0B
    };
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 1); // 1 function body
    // Body size = locals header + code
    std::vector<uint8_t> body_bytes;
    WasmBinaryBuilder::AppendU32(body_bytes, 0); // 0 local declarations
    body_bytes.insert(body_bytes.end(), body_code.begin(), body_code.end());

    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(body_bytes.size()));
    sec_code.insert(sec_code.end(), body_bytes.begin(), body_bytes.end());
    builder.AddSection(10, sec_code);

    // Instantiate
    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* exports = inst->GetExportsObject();
    assert(exports->HasNamedProperty("add"));
    auto* add_fn = static_cast<JSNativeFunction*>(exports->GetNamedProperty("add").AsHeapObject());

    // Test: add(40, 2) === 42
    TaggedValue r1 = add_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(40), TaggedValue::FromSmi(2)});
    assert(r1.IsSmi() && r1.AsSmi() == 42);

    // Test: add(-50, 100) === 50
    TaggedValue r2 = add_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(-50), TaggedValue::FromSmi(100)});
    assert(r2.IsSmi() && r2.AsSmi() == 50);

    std::cout << "  -> PASSED: Wasm add function executed and returned correct results (42 and 50)." << std::endl;
}

// ============================================================================
// Test 3: Control Flow (Block, Loop, BrIf, and Factorial)
// ============================================================================
void Test3_ControlFlowFactorial() {
    std::cout << "[Test 3] Control Flow (Loop, Branching, Conditionals & Factorial)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type: (i32) -> (i32)
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    // Function: 1 func of type 0
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    builder.AddSection(3, sec_func);

    // Export: "fact"
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    WasmBinaryBuilder::AppendString(sec_export, "fact");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 0);
    builder.AddSection(7, sec_export);

    // Code: factorial
    // locals: 1 i32 (acc at local 1)
    // acc = 1
    // block
    //   loop
    //     local.get 0; i32.const 1; i32.le_s; br_if 1 (break to block end if n <= 1)
    //     local.get 1; local.get 0; i32.mul; local.set 1 (acc = acc * n)
    //     local.get 0; i32.const 1; i32.sub; local.set 0 (n = n - 1)
    //     br 0 (repeat loop)
    //   end
    // end
    // local.get 1; end
    std::vector<uint8_t> body_code = {
        0x41, 0x01,       // i32.const 1
        0x21, 0x01,       // local.set 1 (acc = 1)
        0x02, 0x40,       // block (void)
        0x03, 0x40,       // loop (void)
        0x20, 0x00,       // local.get 0
        0x41, 0x01,       // i32.const 1
        0x4C,             // i32.le_s
        0x0D, 0x01,       // br_if 1 (exit block)
        0x20, 0x01,       // local.get 1
        0x20, 0x00,       // local.get 0
        0x6C,             // i32.mul
        0x21, 0x01,       // local.set 1
        0x20, 0x00,       // local.get 0
        0x41, 0x01,       // i32.const 1
        0x6B,             // i32.sub
        0x21, 0x00,       // local.set 0
        0x0C, 0x00,       // br 0 (repeat loop)
        0x0B,             // end (loop)
        0x0B,             // end (block)
        0x20, 0x01,       // local.get 1
        0x0B              // end (func)
    };

    std::vector<uint8_t> body_bytes;
    WasmBinaryBuilder::AppendU32(body_bytes, 1); // 1 local declaration entry
    WasmBinaryBuilder::AppendU32(body_bytes, 1); // 1 count
    body_bytes.push_back(0x7F);                  // type i32
    body_bytes.insert(body_bytes.end(), body_code.begin(), body_code.end());

    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 1);
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(body_bytes.size()));
    sec_code.insert(sec_code.end(), body_bytes.begin(), body_bytes.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* fact_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("fact").AsHeapObject());

    // fact(1) == 1
    TaggedValue f1 = fact_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(1)});
    assert(f1.IsSmi() && f1.AsSmi() == 1);

    // fact(5) == 120
    TaggedValue f5 = fact_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(5)});
    assert(f5.IsSmi() && f5.AsSmi() == 120);

    // fact(6) == 720
    TaggedValue f6 = fact_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(6)});
    assert(f6.IsSmi() && f6.AsSmi() == 720);

    std::cout << "  -> PASSED: Factorial loop with br_if executed cleanly (fact(5)=120, fact(6)=720)." << std::endl;
}

// ============================================================================
// Test 4: Linear Memory Operations (Load, Store, Endianness & Out-of-bounds Trap)
// ============================================================================
void Test4_LinearMemoryOperations() {
    std::cout << "[Test 4] Linear Memory Operations (Load, Store, Endianness & Bounds Checking)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type 0: (i32, i32) -> () (store32)
    // Type 1: (i32) -> (i32) (load32)
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    // Type 0
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x7F); sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    // Type 1
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    // Function Section: func 0 has type 0, func 1 has type 1
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 2);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    builder.AddSection(3, sec_func);

    // Memory Section (ID 5): 1 memory, initial 1, max 2
    std::vector<uint8_t> sec_mem;
    WasmBinaryBuilder::AppendU32(sec_mem, 1);
    sec_mem.push_back(0x01); // has_max flag
    WasmBinaryBuilder::AppendU32(sec_mem, 1); // initial
    WasmBinaryBuilder::AppendU32(sec_mem, 2); // max
    builder.AddSection(5, sec_mem);

    // Export Section: export "mem" (memory 0), "store32" (func 0), "load32" (func 1)
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 3);
    WasmBinaryBuilder::AppendString(sec_export, "mem");
    sec_export.push_back(0x02); // memory kind
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "store32");
    sec_export.push_back(0x00); // func kind
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "load32");
    sec_export.push_back(0x00); // func kind
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    builder.AddSection(7, sec_export);

    // Code Section:
    // func 0: local.get 0; local.get 1; i32.store align=2 offset=0; end
    // func 1: local.get 0; i32.load align=2 offset=0; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 2);

    // Body 0
    std::vector<uint8_t> b0 = {0x00, 0x20, 0x00, 0x20, 0x01, 0x36, 0x02, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());

    // Body 1
    std::vector<uint8_t> b1 = {0x00, 0x20, 0x00, 0x28, 0x02, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b1.size()));
    sec_code.insert(sec_code.end(), b1.begin(), b1.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* store_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("store32").AsHeapObject());
    auto* load_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("load32").AsHeapObject());

    // Store 0x12345678 at offset 32
    store_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(32), TaggedValue::FromSmi(0x12345678)});

    // Load from Wasm
    TaggedValue loaded = load_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(32)});
    assert(loaded.IsSmi() && loaded.AsSmi() == 0x12345678);

    // Verify buffer in JS (little-endian byte inspection)
    auto* mem_obj = static_cast<JSObject*>(inst->GetExportsObject()->GetNamedProperty("mem").AsHeapObject());
    auto* ab = static_cast<JSArrayBuffer*>(mem_obj->GetNamedProperty("buffer").AsHeapObject());
    assert(ab->ByteLength() == 65536);
    const uint8_t* raw = ab->Data();
    assert(raw[32] == 0x78);
    assert(raw[33] == 0x56);
    assert(raw[34] == 0x34);
    assert(raw[35] == 0x12);

    // Test out of bounds access trap
    Interpreter interp;
    Interpreter::SetCurrent(&interp);
    load_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(70000)});
    assert(interp.HasPendingException());
    TaggedValue ex = interp.PendingException();
    interp.ClearPendingException();
    assert(ex.IsHeapObject());
    auto* err_obj = static_cast<JSError*>(ex.AsHeapObject());
    assert(err_obj->Name() == "WebAssembly.RuntimeError");
    Interpreter::SetCurrent(nullptr);

    std::cout << "  -> PASSED: Linear memory load/store verified with little-endian byte layout and trap on OOB." << std::endl;
}

// ============================================================================
// Test 5: Memory Growth (memory.grow in Wasm and JS)
// ============================================================================
void Test5_MemoryGrowth() {
    std::cout << "[Test 5] Memory Growth (WebAssembly.Memory.grow and page size tracking)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type 0: () -> (i32) (size)
    // Type 1: (i32) -> (i32) (grow)
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);

    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    // Functions
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 2);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    builder.AddSection(3, sec_func);

    // Memory: initial 1, max 3
    std::vector<uint8_t> sec_mem;
    WasmBinaryBuilder::AppendU32(sec_mem, 1);
    sec_mem.push_back(0x01);
    WasmBinaryBuilder::AppendU32(sec_mem, 1);
    WasmBinaryBuilder::AppendU32(sec_mem, 3);
    builder.AddSection(5, sec_mem);

    // Export: "mem", "memSize", "memGrow"
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 3);
    WasmBinaryBuilder::AppendString(sec_export, "mem");
    sec_export.push_back(0x02);
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "memSize");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "memGrow");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    builder.AddSection(7, sec_export);

    // Code:
    // func 0 (size): memory.size 0x00; end
    // func 1 (grow): local.get 0; memory.grow 0x00; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 2);

    std::vector<uint8_t> b0 = {0x00, 0x3F, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());

    std::vector<uint8_t> b1 = {0x00, 0x20, 0x00, 0x40, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b1.size()));
    sec_code.insert(sec_code.end(), b1.begin(), b1.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* size_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("memSize").AsHeapObject());
    auto* grow_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("memGrow").AsHeapObject());

    // Initial size is 1 page
    TaggedValue s0 = size_fn->Invoke(TaggedValue::Undefined(), {});
    assert(s0.IsSmi() && s0.AsSmi() == 1);

    // Grow by 1 page via Wasm opcode
    TaggedValue g1 = grow_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(1)});
    assert(g1.IsSmi() && g1.AsSmi() == 1); // returns old size

    // New size is 2 pages
    TaggedValue s1 = size_fn->Invoke(TaggedValue::Undefined(), {});
    assert(s1.IsSmi() && s1.AsSmi() == 2);

    // Grow by 1 page via JS host method on WebAssembly.Memory
    auto* mem_obj = static_cast<JSObject*>(inst->GetExportsObject()->GetNamedProperty("mem").AsHeapObject());
    auto* js_grow_fn = static_cast<JSNativeFunction*>(mem_obj->GetNamedProperty("grow").AsHeapObject());
    TaggedValue g2 = js_grow_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(1)});
    assert(g2.IsSmi() && g2.AsSmi() == 2); // returns old size (2)

    // Now size is 3 pages (the maximum!)
    TaggedValue s2 = size_fn->Invoke(TaggedValue::Undefined(), {});
    assert(s2.IsSmi() && s2.AsSmi() == 3);

    // Growing further beyond maximum must fail (return -1)
    TaggedValue g3 = grow_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(1)});
    assert(g3.IsSmi() && g3.AsSmi() == -1);

    std::cout << "  -> PASSED: Memory grow operations correctly expanded pages and enforced limits." << std::endl;
}

// ============================================================================
// Test 6: Table & Indirect Calls (call_indirect)
// ============================================================================
void Test6_TableAndCallIndirect() {
    std::cout << "[Test 6] Table & Indirect Calls (call_indirect, function dispatch)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type 0: (i32, i32) -> (i32)
    // Type 1: (i32, i32, i32) -> (i32) (dispatch(op, a, b))
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    // Type 0
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x7F); sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    // Type 1
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 3);
    sec_type.push_back(0x7F); sec_type.push_back(0x7F); sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    // Functions:
    // func 0 (sub): type 0
    // func 1 (mul): type 0
    // func 2 (dispatch): type 1
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 3);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    builder.AddSection(3, sec_func);

    // Table: 1 table, funcref, initial 2, max 2
    std::vector<uint8_t> sec_table;
    WasmBinaryBuilder::AppendU32(sec_table, 1);
    sec_table.push_back(0x70); // funcref
    sec_table.push_back(0x01); // has_max
    WasmBinaryBuilder::AppendU32(sec_table, 2);
    WasmBinaryBuilder::AppendU32(sec_table, 2);
    builder.AddSection(4, sec_table);

    // Export: "dispatch"
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    WasmBinaryBuilder::AppendString(sec_export, "dispatch");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 2); // func 2
    builder.AddSection(7, sec_export);

    // Element Section: populate table 0 starting at offset 0 with [0, 1]
    std::vector<uint8_t> sec_elem;
    WasmBinaryBuilder::AppendU32(sec_elem, 1);
    WasmBinaryBuilder::AppendU32(sec_elem, 0); // table 0
    // offset expr: i32.const 0, end
    sec_elem.push_back(0x41); sec_elem.push_back(0x00); sec_elem.push_back(0x0B);
    WasmBinaryBuilder::AppendU32(sec_elem, 2); // 2 funcs
    WasmBinaryBuilder::AppendU32(sec_elem, 0); // func 0 (sub)
    WasmBinaryBuilder::AppendU32(sec_elem, 1); // func 1 (mul)
    builder.AddSection(9, sec_elem);

    // Code:
    // func 0 (sub): local.get 0; local.get 1; i32.sub; end
    // func 1 (mul): local.get 0; local.get 1; i32.mul; end
    // func 2 (dispatch): local.get 1; local.get 2; local.get 0; call_indirect type 0, 0x00; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 3);

    std::vector<uint8_t> b0 = {0x00, 0x20, 0x00, 0x20, 0x01, 0x6B, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());

    std::vector<uint8_t> b1 = {0x00, 0x20, 0x00, 0x20, 0x01, 0x6C, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b1.size()));
    sec_code.insert(sec_code.end(), b1.begin(), b1.end());

    std::vector<uint8_t> b2 = {0x00, 0x20, 0x01, 0x20, 0x02, 0x20, 0x00, 0x11, 0x00, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b2.size()));
    sec_code.insert(sec_code.end(), b2.begin(), b2.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* disp_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("dispatch").AsHeapObject());

    // dispatch(0, 50, 15) -> sub(50, 15) === 35
    TaggedValue r_sub = disp_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(0), TaggedValue::FromSmi(50), TaggedValue::FromSmi(15)});
    assert(r_sub.IsSmi() && r_sub.AsSmi() == 35);

    // dispatch(1, 6, 7) -> mul(6, 7) === 42
    TaggedValue r_mul = disp_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(1), TaggedValue::FromSmi(6), TaggedValue::FromSmi(7)});
    assert(r_mul.IsSmi() && r_mul.AsSmi() == 42);

    std::cout << "  -> PASSED: Indirect function dispatch via table verified." << std::endl;
}

// ============================================================================
// Test 7: Globals (WebAssembly.Global, Getters, Setters, Mutability)
// ============================================================================
void Test7_Globals() {
    std::cout << "[Test 7] Globals (WebAssembly.Global, Mutable and Immutable Exports)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type: () -> ()
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    builder.AddSection(1, sec_type);

    // Func 0
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    builder.AddSection(3, sec_func);

    // Global Section: 1 mutable global i32, init 100
    std::vector<uint8_t> sec_global;
    WasmBinaryBuilder::AppendU32(sec_global, 1);
    sec_global.push_back(0x7F); // i32
    sec_global.push_back(0x01); // mutable
    // init: i32.const 100, end
    sec_global.push_back(0x41);
    WasmBinaryBuilder::AppendI32(sec_global, 100);
    sec_global.push_back(0x0B);
    builder.AddSection(6, sec_global);

    // Export: "gVal" (global 0), "inc" (func 0)
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 2);
    WasmBinaryBuilder::AppendString(sec_export, "gVal");
    sec_export.push_back(0x03); // global kind
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "inc");
    sec_export.push_back(0x00); // func kind
    WasmBinaryBuilder::AppendU32(sec_export, 0);
    builder.AddSection(7, sec_export);

    // Code: global.get 0; i32.const 5; i32.add; global.set 0; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 1);
    std::vector<uint8_t> b0 = {0x00, 0x23, 0x00, 0x41, 0x05, 0x6A, 0x24, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* g_obj = static_cast<JSObject*>(inst->GetExportsObject()->GetNamedProperty("gVal").AsHeapObject());
    assert(g_obj->GetNamedProperty("value").AsSmi() == 100);

    auto* inc_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("inc").AsHeapObject());
    inc_fn->Invoke(TaggedValue::Undefined(), {});

    // Now value in instance global is 105!
    assert(inst->Globals()[0]->Get().i32 == 105);

    // Test standalone WebAssembly.Global constructor
    auto* wasm_obj = static_cast<JSObject*>(realm.GetGlobalProperty("WebAssembly").AsHeapObject());
    auto* g_ctor = static_cast<JSNativeFunction*>(wasm_obj->GetNamedProperty("Global").AsHeapObject());

    auto* desc = AllocateRuntime<JSObject>(realm.GetRootShape());
    desc->SetNamedProperty("value", TaggedValue::FromHeapObject(AllocateRuntime<JSString>("i32")), realm.NextShapeId());
    desc->SetNamedProperty("mutable", TaggedValue::FromBoolean(true), realm.NextShapeId());

    TaggedValue standalone_g = g_ctor->Invoke(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(desc), TaggedValue::FromSmi(42)});
    assert(standalone_g.IsHeapObject());
    auto* sg_obj = static_cast<JSObject*>(standalone_g.AsHeapObject());
    assert(sg_obj->GetNamedProperty("value").AsSmi() == 42);

    std::cout << "  -> PASSED: WebAssembly.Global and mutable exported globals verified." << std::endl;
}

// ============================================================================
// Test 8: Host JS <-> Wasm Bi-directional Call Interoperability
// ============================================================================
void Test8_HostJSCallBoundary() {
    std::cout << "[Test 8] Host JS <-> Wasm Bi-directional Call Interoperability..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type 0: (i32) -> (i32)
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    // Import Section: import "env"."hostSquare" of type 0
    std::vector<uint8_t> sec_import;
    WasmBinaryBuilder::AppendU32(sec_import, 1);
    WasmBinaryBuilder::AppendString(sec_import, "env");
    WasmBinaryBuilder::AppendString(sec_import, "hostSquare");
    sec_import.push_back(0x00); // func kind
    WasmBinaryBuilder::AppendU32(sec_import, 0); // type 0
    builder.AddSection(2, sec_import);

    // Function Section: 1 defined func (type 0)
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    builder.AddSection(3, sec_func);

    // Export: "callHost" (func index 1)
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    WasmBinaryBuilder::AppendString(sec_export, "callHost");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    builder.AddSection(7, sec_export);

    // Code: local.get 0; call 0 (imported func); i32.const 1; i32.add; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 1);
    std::vector<uint8_t> b0 = {0x00, 0x20, 0x00, 0x10, 0x00, 0x41, 0x01, 0x6A, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());
    builder.AddSection(10, sec_code);

    // Host import object: { env: { hostSquare: (x) => x * x } }
    auto* import_obj = AllocateRuntime<JSObject>(realm.GetRootShape());
    auto* env_obj = AllocateRuntime<JSObject>(realm.GetRootShape());
    bool host_was_called = false;
    auto* host_square = AllocateRuntime<JSNativeFunction>("hostSquare", [&host_was_called](const std::vector<TaggedValue>& args) -> TaggedValue {
        host_was_called = true;
        int32_t v = args.empty() ? 0 : args[0].AsSmi();
        return TaggedValue::FromSmi(v * v);
    }, realm.GetRootShape());

    env_obj->SetNamedProperty("hostSquare", TaggedValue::FromHeapObject(host_square), realm.NextShapeId());
    import_obj->SetNamedProperty("env", TaggedValue::FromHeapObject(env_obj), realm.NextShapeId());

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::FromHeapObject(import_obj), realm, &err);
    assert(inst != nullptr);

    auto* call_host_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("callHost").AsHeapObject());

    // Calling with 9: hostSquare(9) is 81, + 1 = 82
    TaggedValue r = call_host_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(9)});
    assert(host_was_called);
    assert(r.IsSmi() && r.AsSmi() == 82);

    std::cout << "  -> PASSED: Wasm successfully invoked host JavaScript callback and received return value." << std::endl;
}

// ============================================================================
// Test 9: Traps & Exceptions (WebAssembly.RuntimeError)
// ============================================================================
void Test9_TrapsAndExceptions() {
    std::cout << "[Test 9] Traps and Exception Handling (divide by zero & unreachable)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Type 0: (i32, i32) -> (i32)
    // Type 1: () -> ()
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x7F); sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);

    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    WasmBinaryBuilder::AppendU32(sec_type, 0);
    builder.AddSection(1, sec_type);

    // Functions
    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 2);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    builder.AddSection(3, sec_func);

    // Export: "div", "boom"
    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 2);
    WasmBinaryBuilder::AppendString(sec_export, "div");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 0);

    WasmBinaryBuilder::AppendString(sec_export, "boom");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    builder.AddSection(7, sec_export);

    // Code:
    // func 0: local.get 0; local.get 1; i32.div_s; end
    // func 1: unreachable; end
    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 2);

    std::vector<uint8_t> b0 = {0x00, 0x20, 0x00, 0x20, 0x01, 0x6D, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());

    std::vector<uint8_t> b1 = {0x00, 0x00, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b1.size()));
    sec_code.insert(sec_code.end(), b1.begin(), b1.end());
    builder.AddSection(10, sec_code);

    std::string err;
    auto mod = WasmModule::Decode(builder.bytes.data(), builder.bytes.size(), &err);
    assert(mod != nullptr);

    auto inst = WasmInstance::Instantiate(mod, TaggedValue::Undefined(), realm, &err);
    assert(inst != nullptr);

    auto* div_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("div").AsHeapObject());
    auto* boom_fn = static_cast<JSNativeFunction*>(inst->GetExportsObject()->GetNamedProperty("boom").AsHeapObject());

    Interpreter interp;
    Interpreter::SetCurrent(&interp);

    // 1. Division by zero trap
    div_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(10), TaggedValue::FromSmi(0)});
    assert(interp.HasPendingException());
    TaggedValue ex1 = interp.PendingException();
    interp.ClearPendingException();
    assert(static_cast<JSError*>(ex1.AsHeapObject())->Name() == "WebAssembly.RuntimeError");

    // 2. Unreachable opcode trap
    boom_fn->Invoke(TaggedValue::Undefined(), {});
    assert(interp.HasPendingException());
    TaggedValue ex2 = interp.PendingException();
    interp.ClearPendingException();
    assert(static_cast<JSError*>(ex2.AsHeapObject())->Name() == "WebAssembly.RuntimeError");
    Interpreter::SetCurrent(nullptr);

    std::cout << "  -> PASSED: Division by zero and unreachable opcodes properly raised WebAssembly.RuntimeError." << std::endl;
}

// ============================================================================
// Test 10: Asynchronous API (WebAssembly.compile, WebAssembly.instantiate)
// ============================================================================
void Test10_AsynchronousAPI() {
    std::cout << "[Test 10] Asynchronous Promises API (WebAssembly.compile, WebAssembly.instantiate)..." << std::endl;
    Agent agent;
    auto& realm = agent.GetRealm();

    WasmBinaryBuilder builder;

    // Simple module with 1 function: mul(i32, i32) -> i32
    std::vector<uint8_t> sec_type;
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x60);
    WasmBinaryBuilder::AppendU32(sec_type, 2);
    sec_type.push_back(0x7F); sec_type.push_back(0x7F);
    WasmBinaryBuilder::AppendU32(sec_type, 1);
    sec_type.push_back(0x7F);
    builder.AddSection(1, sec_type);

    std::vector<uint8_t> sec_func;
    WasmBinaryBuilder::AppendU32(sec_func, 1);
    WasmBinaryBuilder::AppendU32(sec_func, 0);
    builder.AddSection(3, sec_func);

    std::vector<uint8_t> sec_export;
    WasmBinaryBuilder::AppendU32(sec_export, 1);
    WasmBinaryBuilder::AppendString(sec_export, "mul");
    sec_export.push_back(0x00);
    WasmBinaryBuilder::AppendU32(sec_export, 0);
    builder.AddSection(7, sec_export);

    std::vector<uint8_t> sec_code;
    WasmBinaryBuilder::AppendU32(sec_code, 1);
    std::vector<uint8_t> b0 = {0x00, 0x20, 0x00, 0x20, 0x01, 0x6C, 0x0B};
    WasmBinaryBuilder::AppendU32(sec_code, static_cast<uint32_t>(b0.size()));
    sec_code.insert(sec_code.end(), b0.begin(), b0.end());
    builder.AddSection(10, sec_code);

    auto* wasm_obj = static_cast<JSObject*>(realm.GetGlobalProperty("WebAssembly").AsHeapObject());
    auto* compile_fn = static_cast<JSNativeFunction*>(wasm_obj->GetNamedProperty("compile").AsHeapObject());
    auto* instantiate_fn = static_cast<JSNativeFunction*>(wasm_obj->GetNamedProperty("instantiate").AsHeapObject());

    auto* ab = builder.ToArrayBuffer(realm);

    // 1. WebAssembly.compile
    TaggedValue compile_prom_val = compile_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(ab)});
    assert(compile_prom_val.IsHeapObject());
    auto* compile_prom = static_cast<JSPromise*>(compile_prom_val.AsHeapObject());
    assert(compile_prom->State() == PromiseState::Fulfilled);
    TaggedValue compiled_module = compile_prom->Result();
    assert(compiled_module.IsHeapObject());

    // 2. WebAssembly.instantiate with raw buffer
    TaggedValue inst_prom_val = instantiate_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromHeapObject(ab)});
    assert(inst_prom_val.IsHeapObject());
    auto* inst_prom = static_cast<JSPromise*>(inst_prom_val.AsHeapObject());
    assert(inst_prom->State() == PromiseState::Fulfilled);
    TaggedValue result_pair = inst_prom->Result();
    assert(result_pair.IsHeapObject());
    auto* pair_obj = static_cast<JSObject*>(result_pair.AsHeapObject());
    assert(pair_obj->HasNamedProperty("module"));
    assert(pair_obj->HasNamedProperty("instance"));

    auto* inst_obj = static_cast<JSObject*>(pair_obj->GetNamedProperty("instance").AsHeapObject());
    auto* exports_obj = static_cast<JSObject*>(inst_obj->GetNamedProperty("exports").AsHeapObject());
    assert(exports_obj->HasNamedProperty("mul"));

    auto* mul_fn = static_cast<JSNativeFunction*>(exports_obj->GetNamedProperty("mul").AsHeapObject());
    TaggedValue mul_res = mul_fn->Invoke(TaggedValue::Undefined(), {TaggedValue::FromSmi(7), TaggedValue::FromSmi(8)});
    assert(mul_res.IsSmi() && mul_res.AsSmi() == 56);

    std::cout << "  -> PASSED: Async WebAssembly.compile and WebAssembly.instantiate fulfilled with working instances." << std::endl;
}

// ============================================================================
// Main Test Runner
// ============================================================================
int main() {
    std::cout << "============================================================" << std::endl;
    std::cout << "  AetherVM Test Suite - Milestone 19: WebAssembly MVP" << std::endl;
    std::cout << "============================================================" << std::endl;

    Test1_ValidationAndCompilation();
    Test2_BasicAddFunction();
    Test3_ControlFlowFactorial();
    Test4_LinearMemoryOperations();
    Test5_MemoryGrowth();
    Test6_TableAndCallIndirect();
    Test7_Globals();
    Test8_HostJSCallBoundary();
    Test9_TrapsAndExceptions();
    Test10_AsynchronousAPI();

    std::cout << "============================================================" << std::endl;
    std::cout << "  ALL 10/10 MILESTONE 19 WEBASSEMBLY TESTS PASSED!" << std::endl;
    std::cout << "============================================================" << std::endl;
    return 0;
}
