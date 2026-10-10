#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/assembler.h"
#include "aether/baseline_jit.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"

#include <iostream>
#include <chrono>
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

    std::cout << "\n========================================================\n"
              << "   ALL TIER 1 JIT NATIVE TESTS PASSED (4/4)             \n"
              << "========================================================\n" << std::flush;
    return 0;
}
