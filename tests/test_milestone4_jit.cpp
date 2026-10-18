#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/assembler.h"
#include "aether/baseline_jit.h"
#include "aether/interpreter.h"
#include "aether/heap_object.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"

#include <iostream>
#include <chrono>
#include <limits>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestDirectNativeArithmetic() {
    std::cout << "[Test 1] Verifying Direct Native x86-64 Arithmetic Code Generation...\n" << std::flush;

    // Bytecode for:
    // r0 = 15;
    // r1 = 30;
    // acc = r0 + r1; (45)
    // Return;
    BytecodeArray bc(2, 0);
    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(15);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(0);

    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(30);
    bc.EmitOpcode(Opcode::Star);
    bc.EmitU8(1);

    bc.EmitOpcode(Opcode::Ldar);
    bc.EmitU8(0);

    bc.EmitOpcode(Opcode::Add);
    bc.EmitU8(1);
    bc.EmitU16(0);

    bc.EmitOpcode(Opcode::Return);

    // Compile directly to native x86-64 machine code
    auto jit_code = BaselineJIT::Compile(bc);
    assert(jit_code != nullptr);
    assert(jit_code->CodeAddress() != nullptr);

    TaggedValue result = BaselineJIT::Invoke(*jit_code);

    std::cout << "  -> Native machine code executed successfully. Result: " << result.ToString() << "\n" << std::flush;
    assert(result.IsSmi());
    assert(result.AsSmi() == 45);
}

void TestNativeLoopBranching() {
    std::cout << "[Test 2] Verifying Native x86-64 Loops and Conditional Branching...\n" << std::flush;

    // Loop calculating sum 1..10 = 55
    // var count = 10;
    // var total = 0;
    // while (count > 0) {
    //     total = total + count;
    //     count = count - 1;
    // }
    // return total;
    auto program = std::make_unique<BlockStmt>();
    program->AddStatement(std::make_unique<VarDeclStmt>("count", std::make_unique<LiteralExpr>(TaggedValue::FromSmi(10))));
    program->AddStatement(std::make_unique<VarDeclStmt>("total", std::make_unique<LiteralExpr>(TaggedValue::FromSmi(0))));

    auto loop_cond = std::make_unique<BinaryExpr>(
        BinaryOp::GreaterThan,
        std::make_unique<IdentifierExpr>("count"),
        std::make_unique<LiteralExpr>(TaggedValue::FromSmi(0))
    );
    auto loop_body = std::make_unique<BlockStmt>();
    loop_body->AddStatement(std::make_unique<ExprStmt>(
        std::make_unique<AssignExpr>("total", std::make_unique<BinaryExpr>(BinaryOp::Add, std::make_unique<IdentifierExpr>("total"), std::make_unique<IdentifierExpr>("count")))
    ));
    loop_body->AddStatement(std::make_unique<ExprStmt>(
        std::make_unique<AssignExpr>("count", std::make_unique<BinaryExpr>(BinaryOp::Sub, std::make_unique<IdentifierExpr>("count"), std::make_unique<LiteralExpr>(TaggedValue::FromSmi(1))))
    ));
    program->AddStatement(std::make_unique<WhileStmt>(std::move(loop_cond), std::move(loop_body)));
    program->AddStatement(std::make_unique<ReturnStmt>(std::make_unique<IdentifierExpr>("total")));

    BytecodeEmitter emitter;
    auto bc = emitter.Emit(program.get());

    auto jit_code = BaselineJIT::Compile(*bc);
    TaggedValue result = BaselineJIT::Invoke(*jit_code);
    std::cout << "  -> Native loop executed. Result: " << result.ToString() << "\n" << std::flush;
    assert(result.IsSmi());
    assert(result.AsSmi() == 55);
}

void TestNativeParameterizedFunction() {
    std::cout << "[Test 3] Verifying Parameter Passing into Native x86-64 Machine Code...\n" << std::flush;

    // function sumN(n) {
    //     var total = 0;
    //     var i = 1;
    //     while (i <= n) {
    //         total = total + i;
    //         i = i + 1;
    //     }
    //     return total;
    // }
    std::string_view src =
        "function sumN(n) {\n"
        "    var total = 0;\n"
        "    var i = 1;\n"
        "    while (i <= n) {\n"
        "        total = total + i;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return total;\n"
        "}\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();

    BytecodeEmitter emitter;
    auto func_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));

    auto jit_code = BaselineJIT::Compile(*func_bc);

    // Test with n = 100: sum(1..100) = 5050
    TaggedValue args[1] = { TaggedValue::FromSmi(100) };

    auto start = std::chrono::high_resolution_clock::now();
    TaggedValue result = BaselineJIT::Invoke(*jit_code, args, 1);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "  -> Native sumN(100) = " << result.ToString() << " [executed in " << duration_ns << " ns]\n" << std::flush;

    assert(result.IsSmi());
    assert(result.AsSmi() == 5050);
}

void TestHighThroughputNativeExecution() {
    std::cout << "[Test 4] High-Throughput Native Benchmark: 1,000,000 Iterations...\n" << std::flush;

    std::string_view src =
        "function heavy(n) {\n"
        "    var acc = 0;\n"
        "    var i = 0;\n"
        "    while (i < n) {\n"
        "        acc = acc + 3;\n"
        "        i = i + 1;\n"
        "    }\n"
        "    return acc;\n"
        "}\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();

    BytecodeEmitter emitter;
    auto func_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));

    auto jit_code = BaselineJIT::Compile(*func_bc);

    // Run 1,000,000 loop iterations in native code
    TaggedValue args[1] = { TaggedValue::FromSmi(1000000) };

    auto start = std::chrono::high_resolution_clock::now();
    TaggedValue result = BaselineJIT::Invoke(*jit_code, args, 1);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  -> 1,000,000 native loop iterations result: " << result.ToString() 
              << " [completed in " << duration_us << " us / " << (duration_us / 1000.0) << " ms]\n" << std::flush;

    assert(result.IsSmi());
    assert(result.AsSmi() == 3000000);
}

void TestBaselineJITTruthinessSemantics() {
    std::cout << "[Test 5] Verifying Baseline JIT vs Interpreter Truthiness Semantics...\n" << std::flush;

    const char* src = 
        "function testBranch(val) {\n"
        "    if (val) {\n"
        "        return 100;\n"
        "    }\n"
        "    return 200;\n"
        "}\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();

    BytecodeEmitter emitter;
    auto func_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));

    auto jit_code = BaselineJIT::Compile(*func_bc);
    assert(jit_code != nullptr);

    Interpreter interp;

    auto test_val = [&](TaggedValue v, int32_t expected) {
        TaggedValue args[1] = { v };
        TaggedValue jit_res = BaselineJIT::Invoke(*jit_code, args, 1);
        TaggedValue interp_res = interp.Execute(*func_bc, { v });
        assert(jit_res.IsSmi());
        assert(interp_res.IsSmi());
        assert(jit_res.AsSmi() == expected);
        assert(interp_res.AsSmi() == expected);
    };

    // Falsy values: all must take the false branch and return 200
    test_val(TaggedValue::Undefined(), 200);
    test_val(TaggedValue::Null(), 200);
    test_val(TaggedValue::False(), 200);
    test_val(TaggedValue::FromSmi(0), 200);
    test_val(TaggedValue::FromDouble(0.0), 200);
    test_val(TaggedValue::FromDouble(std::numeric_limits<double>::quiet_NaN()), 200);

    // Create empty string
    auto empty_str = std::make_unique<JSString>("");
    test_val(TaggedValue::FromHeapObject(empty_str.get()), 200);

    // Truthy values: all must take the true branch and return 100
    test_val(TaggedValue::True(), 100);
    test_val(TaggedValue::FromSmi(1), 100);
    test_val(TaggedValue::FromSmi(-1), 100);
    test_val(TaggedValue::FromDouble(3.14), 100);

    auto non_empty_str = std::make_unique<JSString>("hello");
    test_val(TaggedValue::FromHeapObject(non_empty_str.get()), 100);

    auto obj = std::make_unique<JSObject>(nullptr);
    test_val(TaggedValue::FromHeapObject(obj.get()), 100);

    std::cout << "  -> All JS falsy and truthy values produce identical results across JIT and Interpreter.\n" << std::flush;
}

void TestBaselineJITTypeGuardsAndOverflow() {
    std::cout << "[Test 6] Verifying Baseline JIT Type Guards and Overflow Deoptimization Fallback...\n" << std::flush;

    const char* src =
        "function addNums(a, b) {\n"
        "    return a + b;\n"
        "}\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();

    BytecodeEmitter emitter;
    auto func_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));
    assert(func_bc != nullptr);

    Interpreter interp;

    // Trigger Tier 1 compilation
    for (uint32_t i = 0; i < BaselineJIT::kInvocationThreshold + 5; ++i) {
        interp.Execute(*func_bc, { TaggedValue::FromSmi(10), TaggedValue::FromSmi(20) });
    }
    assert(func_bc->GetTier1Code() != nullptr);

    // 1. Valid Smis execute directly
    TaggedValue res1 = interp.Execute(*func_bc, { TaggedValue::FromSmi(15), TaggedValue::FromSmi(25) });
    assert(res1.IsSmi() && res1.AsSmi() == 40);

    // 2. Non-Smi operands trigger deoptimization and fall back cleanly to interpreter
    TaggedValue res_double = interp.Execute(*func_bc, { TaggedValue::FromDouble(2.5), TaggedValue::FromDouble(3.5) });
    assert(res_double.IsDouble() && res_double.AsDouble() == 6.0);

    // 3. Overflow triggers deoptimization and falls back to interpreter double addition
    TaggedValue max_smi = TaggedValue::FromSmi(2147483647);
    TaggedValue one = TaggedValue::FromSmi(1);
    TaggedValue res_overflow = interp.Execute(*func_bc, { max_smi, one });
    assert(res_overflow.IsDouble() && res_overflow.AsDouble() == 2147483648.0);

    // 4. Non-integer division and division by zero
    const char* div_src =
        "function divNums(a, b) {\n"
        "    return a / b;\n"
        "}\n";
    Lexer lexer_div(div_src);
    Parser parser_div(lexer_div);
    auto div_decl = parser_div.ParseStatement();
    auto div_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(div_decl.get()));
    for (uint32_t i = 0; i < BaselineJIT::kInvocationThreshold + 5; ++i) {
        interp.Execute(*div_bc, { TaggedValue::FromSmi(20), TaggedValue::FromSmi(4) });
    }
    assert(div_bc->GetTier1Code() != nullptr);

    TaggedValue res_div_exact = interp.Execute(*div_bc, { TaggedValue::FromSmi(20), TaggedValue::FromSmi(4) });
    assert(res_div_exact.IsSmi() && res_div_exact.AsSmi() == 5);

    // Non-integer division deopts to float
    TaggedValue res_div_float = interp.Execute(*div_bc, { TaggedValue::FromSmi(5), TaggedValue::FromSmi(2) });
    assert(res_div_float.IsDouble() && res_div_float.AsDouble() == 2.5);

    // Division by zero deopts to Infinity
    TaggedValue res_div_zero = interp.Execute(*div_bc, { TaggedValue::FromSmi(1), TaggedValue::FromSmi(0) });
    assert(res_div_zero.IsDouble() && std::isinf(res_div_zero.AsDouble()));

    // 0 / -1 in JS produces -0.0, which deopts to double -0.0
    TaggedValue res_div_negzero = interp.Execute(*div_bc, { TaggedValue::FromSmi(0), TaggedValue::FromSmi(-1) });
    assert(res_div_negzero.IsDouble() && res_div_negzero.AsDouble() == 0.0 && std::signbit(res_div_negzero.AsDouble()));

    // INT32_MIN % -1 must deopt before x86 idiv's hardware overflow trap and
    // preserve the ECMAScript negative-zero result.
    const char* mod_src =
        "function modNums(a, b) {\n"
        "    return a % b;\n"
        "}\n";
    Lexer lexer_mod(mod_src);
    Parser parser_mod(lexer_mod);
    auto mod_decl = parser_mod.ParseStatement();
    auto mod_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(mod_decl.get()));
    for (uint32_t i = 0; i < BaselineJIT::kInvocationThreshold + 5; ++i) {
        interp.Execute(*mod_bc, { TaggedValue::FromSmi(10), TaggedValue::FromSmi(3) });
    }
    TaggedValue res_mod_overflow = interp.Execute(*mod_bc, { TaggedValue::FromSmi(INT32_MIN), TaggedValue::FromSmi(-1) });
    assert(res_mod_overflow.IsDouble() && res_mod_overflow.AsDouble() == 0.0 && std::signbit(res_mod_overflow.AsDouble()));

    // 5. Multiplication: 0 * -1 produces -0.0, which deopts to double -0.0
    const char* mul_src =
        "function mulNums(a, b) {\n"
        "    return a * b;\n"
        "}\n";
    Lexer lexer_mul(mul_src);
    Parser parser_mul(lexer_mul);
    auto mul_decl = parser_mul.ParseStatement();
    auto mul_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(mul_decl.get()));
    for (uint32_t i = 0; i < BaselineJIT::kInvocationThreshold + 5; ++i) {
        interp.Execute(*mul_bc, { TaggedValue::FromSmi(3), TaggedValue::FromSmi(4) });
    }
    assert(mul_bc->GetTier1Code() != nullptr);

    TaggedValue res_mul_negzero = interp.Execute(*mul_bc, { TaggedValue::FromSmi(0), TaggedValue::FromSmi(-1) });
    assert(res_mul_negzero.IsDouble() && res_mul_negzero.AsDouble() == 0.0 && std::signbit(res_mul_negzero.AsDouble()));

    std::cout << "  -> Baseline JIT type guards, overflow checks, -0.0 preservation, and deoptimization fallback verified.\n" << std::flush;
}

void TestBaselineJITMissingArguments() {
    std::cout << "[Test 7] Verifying Omitted Arguments Become Undefined Without Native Faults...\n" << std::flush;

    const char* src =
        "function identity(value) { return value; }\n";
    Lexer lexer(src);
    Parser parser(lexer);
    auto func_decl = parser.ParseStatement();
    BytecodeEmitter emitter;
    auto func_bc = emitter.EmitFunction(dynamic_cast<FunctionDeclStmt*>(func_decl.get()));
    auto jit_code = BaselineJIT::Compile(*func_bc);
    assert(jit_code != nullptr);

    TaggedValue result = BaselineJIT::Invoke(*jit_code, nullptr, 0);
    assert(result.IsUndefined());
    std::cout << "  -> Native entry handled an omitted parameter as undefined.\n" << std::flush;
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 4 (Tier 1 Baseline JIT) Test Suite\n"
              << "========================================================\n\n" << std::flush;

    TestDirectNativeArithmetic();
    std::cout << "\n" << std::flush;
    TestNativeLoopBranching();
    std::cout << "\n" << std::flush;
    TestNativeParameterizedFunction();
    std::cout << "\n" << std::flush;
    TestHighThroughputNativeExecution();
    std::cout << "\n" << std::flush;
    TestBaselineJITTruthinessSemantics();
    std::cout << "\n" << std::flush;
    TestBaselineJITTypeGuardsAndOverflow();
    std::cout << "\n" << std::flush;
    TestBaselineJITMissingArguments();

    std::cout << "\n========================================================\n"
              << "   ALL TIER 1 JIT NATIVE TESTS PASSED (7/7)             \n"
              << "========================================================\n" << std::flush;
    return 0;
}
