#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/assembler.h"
#include "aether/baseline_jit.h"
#include "aether/sea_of_nodes.h"
#include "aether/optimizing_jit.h"
#include "aether/interpreter.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>
#include <vector>
#include <memory>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

TaggedValue RunScript(const std::string& source) {
    Agent agent;
    return agent.EvaluateScript(source);
}

void TestGenuineComparisonsAndTypeTests() {
    std::cout << "[Test 1] Verifying TestNotEqual, TestStrictNotEqual, TestNull, TestUndefined, TestTypeOf...\n" << std::flush;

    Interpreter interp;

    // 1. TestNotEqual & TestStrictNotEqual
    {
        BytecodeArray bc(1, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(42);
        bc.EmitOpcode(Opcode::Star);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(42);
        bc.EmitOpcode(Opcode::TestNotEqual);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsBoolean() && res_interp.ToBoolean() == false);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsBoolean() && res_jit.ToBoolean() == false);
    }

    {
        BytecodeArray bc(1, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(43);
        bc.EmitOpcode(Opcode::Star);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(42);
        bc.EmitOpcode(Opcode::TestNotEqual);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsBoolean() && res_interp.ToBoolean() == true);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsBoolean() && res_jit.ToBoolean() == true);
    }

    {
        BytecodeArray bc(1, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(42);
        bc.EmitOpcode(Opcode::Star);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(42);
        bc.EmitOpcode(Opcode::TestStrictNotEqual);
        bc.EmitU8(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsBoolean() && res_interp.ToBoolean() == false);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsBoolean() && res_jit.ToBoolean() == false);
    }

    // 2. TestNull & TestUndefined
    {
        BytecodeArray bc_null(0, 0);
        bc_null.EmitOpcode(Opcode::LdaNull);
        bc_null.EmitOpcode(Opcode::TestNull);
        bc_null.EmitOpcode(Opcode::Return);

        TaggedValue res1 = interp.Execute(bc_null);
        assert(res1.IsBoolean() && res1.ToBoolean() == true);

        auto jit_null = BaselineJIT::Compile(bc_null);
        assert(jit_null != nullptr);
        TaggedValue res1_jit = BaselineJIT::Invoke(*jit_null);
        assert(res1_jit.IsBoolean() && res1_jit.ToBoolean() == true);

        BytecodeArray bc_not_null(0, 0);
        bc_not_null.EmitOpcode(Opcode::LdaUndefined);
        bc_not_null.EmitOpcode(Opcode::TestNull);
        bc_not_null.EmitOpcode(Opcode::Return);

        TaggedValue res2 = interp.Execute(bc_not_null);
        assert(res2.IsBoolean() && res2.ToBoolean() == false);

        auto jit_not_null = BaselineJIT::Compile(bc_not_null);
        assert(jit_not_null != nullptr);
        TaggedValue res2_jit = BaselineJIT::Invoke(*jit_not_null);
        assert(res2_jit.IsBoolean() && res2_jit.ToBoolean() == false);
    }

    {
        BytecodeArray bc_undef(0, 0);
        bc_undef.EmitOpcode(Opcode::LdaUndefined);
        bc_undef.EmitOpcode(Opcode::TestUndefined);
        bc_undef.EmitOpcode(Opcode::Return);

        TaggedValue res1 = interp.Execute(bc_undef);
        assert(res1.IsBoolean() && res1.ToBoolean() == true);

        auto jit_undef = BaselineJIT::Compile(bc_undef);
        assert(jit_undef != nullptr);
        TaggedValue res1_jit = BaselineJIT::Invoke(*jit_undef);
        assert(res1_jit.IsBoolean() && res1_jit.ToBoolean() == true);

        BytecodeArray bc_not_undef(0, 0);
        bc_not_undef.EmitOpcode(Opcode::LdaSmi);
        bc_not_undef.EmitI32(0);
        bc_not_undef.EmitOpcode(Opcode::TestUndefined);
        bc_not_undef.EmitOpcode(Opcode::Return);

        TaggedValue res2 = interp.Execute(bc_not_undef);
        assert(res2.IsBoolean() && res2.ToBoolean() == false);

        auto jit_not_undef = BaselineJIT::Compile(bc_not_undef);
        assert(jit_not_undef != nullptr);
        TaggedValue res2_jit = BaselineJIT::Invoke(*jit_not_undef);
        assert(res2_jit.IsBoolean() && res2_jit.ToBoolean() == false);
    }

    // 3. TestTypeOf
    {
        BytecodeArray bc(0, 1);
        uint16_t c_idx = bc.AddConstant(TaggedValue::FromHeapObject(AllocateRuntime<JSString>("number")));
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(1234);
        bc.EmitOpcode(Opcode::TestTypeOf);
        bc.EmitU16(c_idx);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res = interp.Execute(bc);
        assert(res.IsBoolean() && res.ToBoolean() == true);
    }

    std::cout << "  -> TestNotEqual, TestStrictNotEqual, TestNull, TestUndefined, TestTypeOf passed.\n" << std::flush;
}

void TestSmiImmediateBinaryOps() {
    std::cout << "[Test 2] Verifying Smi-Immediate Arithmetic and Shifts across Tiers...\n" << std::flush;

    Interpreter interp;

    // 1. AddSmi & SubSmi
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(100);
        bc.EmitOpcode(Opcode::AddSmi);
        bc.EmitI32(50);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::SubSmi);
        bc.EmitI32(25);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 125);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 125);
    }

    // 2. MulSmi & DivSmi & ModSmi
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(10);
        bc.EmitOpcode(Opcode::MulSmi);
        bc.EmitI32(6);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::DivSmi);
        bc.EmitI32(4);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::ModSmi);
        bc.EmitI32(10);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 5);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 5);
    }

    // 3. BitwiseAndSmi, BitwiseOrSmi, BitwiseXorSmi
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(0b1100);
        bc.EmitOpcode(Opcode::BitwiseAndSmi);
        bc.EmitI32(0b1010);
        bc.EmitOpcode(Opcode::BitwiseOrSmi);
        bc.EmitI32(0b0001);
        bc.EmitOpcode(Opcode::BitwiseXorSmi);
        bc.EmitI32(0b0011);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 10);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 10);
    }

    // 4. ShiftLeftSmi, ShiftRightSmi, ShiftRightUnsignedSmi
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(3);
        bc.EmitOpcode(Opcode::ShiftLeftSmi);
        bc.EmitU8(4);
        bc.EmitOpcode(Opcode::ShiftRightSmi);
        bc.EmitU8(2);
        bc.EmitOpcode(Opcode::ShiftRightUnsignedSmi);
        bc.EmitU8(1);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 6);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 6);
    }

    // 5. AddSmi string concatenation test
    {
        BytecodeArray bc(0, 1);
        uint16_t c_idx = bc.AddConstant(TaggedValue::FromHeapObject(AllocateRuntime<JSString>("prefix-")));
        bc.EmitOpcode(Opcode::LdaConstant);
        bc.EmitU16(c_idx);
        bc.EmitOpcode(Opcode::AddSmi);
        bc.EmitI32(99);
        bc.EmitU16(0);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res = interp.Execute(bc);
        assert(res.IsHeapObject() && res.AsHeapObject()->IsString());
        assert(static_cast<JSString*>(res.AsHeapObject())->Value() == "prefix-99");
    }

    std::cout << "  -> Smi-Immediate arithmetic and shift operations verified.\n" << std::flush;
}

void TestFastConditionalBranches() {
    std::cout << "[Test 3] Verifying Fast Conditional Branches (JumpIfNull, JumpIfUndefined, etc.)...\n" << std::flush;

    Interpreter interp;

    // JumpIfNull
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaNull);
        bc.EmitOpcode(Opcode::JumpIfNull);
        bc.EmitI32(6);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(22);
        bc.EmitOpcode(Opcode::Return);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(11);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 11);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 11);
    }

    // JumpIfNotNull
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(5);
        bc.EmitOpcode(Opcode::JumpIfNotNull);
        bc.EmitI32(6);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(44);
        bc.EmitOpcode(Opcode::Return);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(33);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 33);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 33);
    }

    // JumpIfUndefined & JumpIfNotUndefined
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaUndefined);
        bc.EmitOpcode(Opcode::JumpIfUndefined);
        bc.EmitI32(6);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(66);
        bc.EmitOpcode(Opcode::Return);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(77);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 77);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 77);
    }

    // JumpIfNullish & JumpIfNotNullish
    {
        BytecodeArray bc(0, 0);
        bc.EmitOpcode(Opcode::LdaNull);
        bc.EmitOpcode(Opcode::JumpIfNullish);
        bc.EmitI32(6);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(88);
        bc.EmitOpcode(Opcode::Return);
        bc.EmitOpcode(Opcode::LdaSmi);
        bc.EmitI32(99);
        bc.EmitOpcode(Opcode::Return);

        TaggedValue res_interp = interp.Execute(bc);
        assert(res_interp.IsSmi() && res_interp.AsSmi() == 99);

        auto jit_code = BaselineJIT::Compile(bc);
        assert(jit_code != nullptr);
        TaggedValue res_jit = BaselineJIT::Invoke(*jit_code);
        assert(res_jit.IsSmi() && res_jit.AsSmi() == 99);
    }

    std::cout << "  -> Fast conditional branches verified.\n" << std::flush;
}

void TestContextAccessAndLiterals() {
    std::cout << "[Test 4] Verifying Context Slots, Empty Array Literals, and RegExp Literals...\n" << std::flush;

    TaggedValue res1 = RunScript(R"(
        function outer() {
            var x = 10;
            function inner() {
                x = x + 5;
                return x;
            }
            return inner();
        }
        outer();
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 15);

    TaggedValue res2 = RunScript(R"(
        var a = [];
        a.push(1);
        a.push(2);
        a.length;
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 2);

    TaggedValue res3 = RunScript(R"(
        var re = /abc/i;
        re.test("ABC");
    )");
    assert(res3.IsBoolean() && res3.ToBoolean() == true);

    std::cout << "  -> Context slots, empty array literals, and regexp literals verified.\n" << std::flush;
}

void TestForInIteration() {
    std::cout << "[Test 5] Verifying ForInEnumerate and ForInNext Bytecodes...\n" << std::flush;

    Agent agent;
    TaggedValue res = agent.EvaluateScript(R"(
        let obj = { a: 1, b: 2, c: 3 };
        let keys = "";
        for (let k in obj) {
            keys = keys + k;
        }
        keys;
    )");
    assert(res.IsHeapObject() && res.AsHeapObject()->IsString());
    std::string str = static_cast<JSString*>(res.AsHeapObject())->Value();
    assert(str.find("a") != std::string::npos);
    assert(str.find("b") != std::string::npos);
    assert(str.find("c") != std::string::npos);

    // Also directly verify ForInEnumerate & ForInNext bytecodes in Interpreter
    Interpreter interp;
    auto* target = AllocateRuntime<JSObject>(interp.GetRootShape());
    uint32_t sid = 1;
    target->SetNamedProperty("x", TaggedValue::FromSmi(10), sid);
    target->SetNamedProperty("y", TaggedValue::FromSmi(20), sid);

    BytecodeArray bc(4, 0); // r0 = target, r1 = enum_arr, r2 = idx (0), r3 = key
    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(0);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(2); // idx = 0

    // ForInEnumerate
    uint16_t c_target = bc.AddConstant(TaggedValue::FromHeapObject(target));
    bc.EmitOpcode(Opcode::LdaConstant);
    bc.EmitU16(c_target);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(0); // r0 = target

    bc.EmitOpcode(Opcode::ForInEnumerate);
    bc.EmitU8(0);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(1); // r1 = enum_arr

    // ForInNext: get first key at idx=0
    bc.EmitOpcode(Opcode::ForInNext);
    bc.EmitU8(0); // obj_reg
    bc.EmitU8(1); // enum_reg
    bc.EmitU8(2); // idx_reg
    bc.EmitOpcode(Opcode::Return);

    TaggedValue key0 = interp.Execute(bc);
    assert(key0.IsHeapObject() && key0.AsHeapObject()->IsString());
    std::string k0_str = static_cast<JSString*>(key0.AsHeapObject())->Value();
    assert(k0_str == "x" || k0_str == "y");

    std::cout << "  -> ForIn iteration verified.\n" << std::flush;
}

void TestTier2SeaOfNodesOptimizedLoop() {
    std::cout << "[Test 6] Verifying Tier 2 Sea-of-Nodes JIT Optimization with Smi math & shifts...\n" << std::flush;

    Interpreter interp;

    BytecodeArray loop_bc(3, 0);
    // r0 = 0 (sum)
    loop_bc.EmitOpcode(Opcode::LdaSmi);
    loop_bc.EmitI32(0);
    loop_bc.EmitOpcode(Opcode::Star);
    loop_bc.EmitU8(0);
    // r1 = 0 (i)
    loop_bc.EmitOpcode(Opcode::LdaSmi);
    loop_bc.EmitI32(0);
    loop_bc.EmitOpcode(Opcode::Star);
    loop_bc.EmitU8(1);
    // r2 = 50 (limit)
    loop_bc.EmitOpcode(Opcode::LdaSmi);
    loop_bc.EmitI32(50);
    loop_bc.EmitOpcode(Opcode::Star);
    loop_bc.EmitU8(2);

    // Loop Header:
    int32_t header_pc = static_cast<int32_t>(loop_bc.CurrentOffset());
    loop_bc.EmitOpcode(Opcode::Ldar);
    loop_bc.EmitU8(1);
    loop_bc.EmitOpcode(Opcode::TestLessThan);
    loop_bc.EmitU8(2);
    size_t jmp_false_pos = loop_bc.CurrentOffset();
    loop_bc.EmitOpcode(Opcode::JumpIfFalse);
    loop_bc.EmitI32(0);

    // Loop Body:
    loop_bc.EmitOpcode(Opcode::Ldar);
    loop_bc.EmitU8(0);
    loop_bc.EmitOpcode(Opcode::AddSmi);
    loop_bc.EmitI32(3);
    loop_bc.EmitU16(0);
    loop_bc.EmitOpcode(Opcode::BitwiseXorSmi);
    loop_bc.EmitI32(1);
    loop_bc.EmitOpcode(Opcode::Star);
    loop_bc.EmitU8(0);

    loop_bc.EmitOpcode(Opcode::Ldar);
    loop_bc.EmitU8(1);
    loop_bc.EmitOpcode(Opcode::AddSmi);
    loop_bc.EmitI32(1);
    loop_bc.EmitU16(0);
    loop_bc.EmitOpcode(Opcode::Star);
    loop_bc.EmitU8(1);

    // Jump back to loop header
    int32_t current_pc = static_cast<int32_t>(loop_bc.CurrentOffset() + 5);
    int32_t back_offset = header_pc - current_pc;
    loop_bc.EmitOpcode(Opcode::Jump);
    loop_bc.EmitI32(back_offset);

    // Loop Exit:
    size_t exit_pc = loop_bc.CurrentOffset();
    int32_t exit_offset = static_cast<int32_t>(exit_pc - (jmp_false_pos + 5));
    loop_bc.PatchI32(jmp_false_pos + 1, exit_offset);

    loop_bc.EmitOpcode(Opcode::Ldar);
    loop_bc.EmitU8(0);
    loop_bc.EmitOpcode(Opcode::Return);

    // 1. Interpreter execution
    TaggedValue interp_res = interp.Execute(loop_bc);
    assert(interp_res.IsSmi());
    int32_t expected_val = interp_res.AsSmi();

    // 2. Baseline JIT compilation & execution
    auto bjit = BaselineJIT::Compile(loop_bc);
    assert(bjit != nullptr);
    TaggedValue bjit_res = BaselineJIT::Invoke(*bjit);
    assert(bjit_res.IsSmi() && bjit_res.AsSmi() == expected_val);

    // 3. Sea-of-Nodes IR Builder
    SeaOfNodesBuilder builder(loop_bc);
    auto graph = builder.Build();
    assert(graph != nullptr);
    assert(graph->Start() != nullptr);

    // 4. Optimizing JIT compilation & execution
    assert(OptimizingJIT::CanCompile(loop_bc));
    auto opt_jit = OptimizingJIT::Compile(loop_bc);
    assert(opt_jit != nullptr);
    TaggedValue opt_res = BaselineJIT::Invoke(*opt_jit);
    assert(opt_res.IsSmi() && opt_res.AsSmi() == expected_val);

    std::cout << "  -> Loop result = " << expected_val << " verified identically across Tier 0, Tier 1, and Tier 2!\n" << std::flush;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "Running AetherVM Genuine Bytecodes & Multi-Tier JIT Suite\n";
    std::cout << "========================================================\n\n";

    TestGenuineComparisonsAndTypeTests();
    TestSmiImmediateBinaryOps();
    TestFastConditionalBranches();
    TestContextAccessAndLiterals();
    TestForInIteration();
    TestTier2SeaOfNodesOptimizedLoop();

    std::cout << "\n========================================================\n";
    std::cout << "ALL 6 GENUINE BYTECODE & JIT SUITES PASSED CLEANLY (6/6)!\n";
    std::cout << "========================================================\n";
    return 0;
}
