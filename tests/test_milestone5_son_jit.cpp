#include "aether/common.h"
#include "aether/value.h"
#include "aether/bytecode.h"
#include "aether/sea_of_nodes.h"
#include "aether/optimizing_jit.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"

#include <iostream>
#include <chrono>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestSeaOfNodesGraphConstruction() {
    std::cout << "[Test 1] Verifying Sea-of-Nodes SSA Graph Construction...\n" << std::flush;

    // Bytecode:
    // r0 = 20;
    // r1 = 30;
    // acc = r0 + r1; (50)
    // Return;
    BytecodeArray bc(2, 0);
    bc.EmitOpcode(Opcode::LdaSmi);
    bc.EmitI32(20);
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
    bc.EmitU16(1);

    bc.EmitOpcode(Opcode::Return);

    SeaOfNodesBuilder builder(bc);
    auto graph = builder.Build();

    assert(graph != nullptr);
    assert(graph->Start() != nullptr);
    assert(graph->StartEffect() != nullptr);
    assert(graph->ReturnNode() != nullptr);

    graph->Dump(std::cout);
    std::cout << "  -> Sea-of-Nodes SSA Graph constructed successfully.\n" << std::flush;
}

void TestGVNAndConstantFolding() {
    std::cout << "[Test 2] Verifying Global Value Numbering (GVN) & Constant Folding...\n" << std::flush;

    Graph graph;
    // Create two identical constant nodes: GVN should deduplicate them
    Node* c1 = graph.NewConstantInt32(42);
    Node* c2 = graph.NewConstantInt32(42);
    assert(c1 == c2 && "GVN must return identical pointer for congruent constants");
    (void)c1; (void)c2;

    // Test Constant Folding: 15 + 25 = 40
    Node* a = graph.NewConstantInt32(15);
    Node* b = graph.NewConstantInt32(25);
    Node* add_node = graph.NewNode(NodeOp::Int32Add);
    add_node->AddInput(a);
    add_node->AddInput(b);

    SeaOfNodesOptimizer::ConstantFolding(graph);

    assert(add_node->Op() == NodeOp::ConstantInt32);
    assert(add_node->IntValue() == 40);
    std::cout << "  -> Constant folding evaluated (15 + 25) -> " << add_node->IntValue() << " at compile time.\n" << std::flush;
}

void TestDeadCodeElimination() {
    std::cout << "[Test 3] Verifying Dead Code Elimination (DCE)...\n" << std::flush;

    Graph graph;
    Node* live_val = graph.NewConstantInt32(100);
    Node* dead_val = graph.NewConstantInt32(999);
    (void)dead_val;

    Node* ret = graph.NewNode(NodeOp::Return);
    ret->AddInput(graph.Start());
    ret->AddInput(graph.StartEffect());
    ret->AddInput(live_val);
    graph.SetReturnNode(ret);

    SeaOfNodesOptimizer::DeadCodeElimination(graph);

    assert(!live_val->IsDead());
    assert(dead_val->IsDead() && "Unused node must be pruned by Dead Code Elimination");
    std::cout << "  -> DCE eliminated unreferenced nodes from graph successfully.\n" << std::flush;
}

void TestTier2OptimizedExecution() {
    std::cout << "[Test 4] Verifying Tier 2 Native Optimized Execution (sumN)...\n" << std::flush;

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

    // Compile using Tier 2 Sea-of-Nodes Optimizing JIT
    auto opt_code = OptimizingJIT::Compile(*func_bc);
    assert(opt_code != nullptr);

    TaggedValue args[1] = { TaggedValue::FromSmi(100) };

    auto start = std::chrono::high_resolution_clock::now();
    TaggedValue result = OptimizingJIT::Invoke(*opt_code, args, 1);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    std::cout << "  -> Tier 2 Optimizing JIT sumN(100) = " << result.ToString() 
              << " [executed in " << duration_ns << " ns]\n" << std::flush;

    assert(result.IsSmi());
    assert(result.AsSmi() == 5050);
}

void TestTier2PeakThroughputBenchmark() {
    std::cout << "[Test 5] Tier 2 Peak Throughput Benchmark: 1,000,000 Unboxed Iterations...\n" << std::flush;

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

    auto opt_code = OptimizingJIT::Compile(*func_bc);
    assert(opt_code != nullptr);

    TaggedValue args[1] = { TaggedValue::FromSmi(1000000) };

    auto start = std::chrono::high_resolution_clock::now();
    TaggedValue result = OptimizingJIT::Invoke(*opt_code, args, 1);
    auto end = std::chrono::high_resolution_clock::now();

    auto duration_us = std::chrono::duration_cast<std::chrono::microseconds>(end - start).count();
    std::cout << "  -> 1,000,000 loop iterations in Tier 2 Sea-of-Nodes: " << result.ToString()
              << " [completed in " << duration_us << " us / " << (duration_us / 1000.0) << " ms]\n" << std::flush;

    assert(result.IsSmi());
    assert(result.AsSmi() == 3000000);
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 5 (Tier 2 Sea-of-Nodes JIT) Tests  \n"
              << "========================================================\n\n" << std::flush;

    TestSeaOfNodesGraphConstruction();
    std::cout << "\n" << std::flush;
    TestGVNAndConstantFolding();
    std::cout << "\n" << std::flush;
    TestDeadCodeElimination();
    std::cout << "\n" << std::flush;
    TestTier2OptimizedExecution();
    std::cout << "\n" << std::flush;
    TestTier2PeakThroughputBenchmark();

    std::cout << "\n========================================================\n"
              << "   ALL TIER 2 OPTIMIZING JIT TESTS PASSED (5/5)         \n"
              << "========================================================\n" << std::flush;
    return 0;
}
