#include "aether/common.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include "aether/interpreter.h"

#include <iostream>
#include <cassert>

using namespace aether;

void TestLexer() {
    std::cout << "[Test 1] Verifying ECMAScript SIMD-Ready Lexer Tokenizer...\n";

    std::string_view code = 
        "var x = 42;\n"
        "let y = 3.14;\n"
        "const msg = \"hello \\\"world\\\"\";\n"
        "if (x >= 40 && !false) {\n"
        "    return x + y;\n"
        "}\n";

    Lexer lexer(code);
    std::vector<TokenKind> expected_kinds = {
        TokenKind::Var, TokenKind::Identifier, TokenKind::Equal, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Let, TokenKind::Identifier, TokenKind::Equal, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Const, TokenKind::Identifier, TokenKind::Equal, TokenKind::String, TokenKind::Semicolon,
        TokenKind::If, TokenKind::OpenParen, TokenKind::Identifier, TokenKind::GreaterEqual, TokenKind::Number,
        TokenKind::AmpAmp, TokenKind::Exclamation, TokenKind::False, TokenKind::CloseParen, TokenKind::OpenBrace,
        TokenKind::Return, TokenKind::Identifier, TokenKind::Plus, TokenKind::Identifier, TokenKind::Semicolon,
        TokenKind::CloseBrace,
        TokenKind::EndOfFile
    };

    size_t idx = 0;
    while (true) {
        Token tok = lexer.NextToken();
        assert(idx < expected_kinds.size());
        assert(tok.kind == expected_kinds[idx]);
        idx++;
        if (tok.kind == TokenKind::EndOfFile) break;
    }

    std::cout << "  -> Lexer tokenized keywords, identifiers, literals, and multi-char operators correctly.\n";
}

void TestParserAST() {
    std::cout << "[Test 2] Verifying Pratt Operator Precedence Expression Parsing...\n";

    std::string_view expr_src = "1 + 2 * 3 == 7";
    Lexer lexer(expr_src);
    Parser parser(lexer);

    auto expr = parser.ParseExpression();
    assert(expr != nullptr);

    // Verify root is BinaryExpr(Equal)
    auto* root = dynamic_cast<BinaryExpr*>(expr.get());
    assert(root != nullptr);
    assert(root->Op() == BinaryOp::Equal);

    // Left child is BinaryExpr(Add, 1, BinaryExpr(Mul, 2, 3))
    auto* lhs = dynamic_cast<BinaryExpr*>(root->Lhs());
    assert(lhs != nullptr);
    assert(lhs->Op() == BinaryOp::Add);

    auto* mul_node = dynamic_cast<BinaryExpr*>(lhs->Rhs());
    assert(mul_node != nullptr);
    assert(mul_node->Op() == BinaryOp::Mul);
    (void)mul_node;

    std::cout << "  -> Precedence tree correctly parsed (* higher precedence than +, + higher than ==).\n";
}

void TestRecursiveFactorial() {
    std::cout << "[Test 3] End-to-End: Parsing & Executing Recursive Factorial...\n";

    std::string_view src =
        "function factorial(n) {\n"
        "    if (n <= 1) {\n"
        "        return 1;\n"
        "    }\n"
        "    return n * factorial(n - 1);\n"
        "}\n"
        "return factorial(6);\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto program = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bytecode = emitter.Emit(program.get());

    std::cout << bytecode->Disassemble() << "\n";

    Interpreter vm;
    TaggedValue result = vm.Execute(*bytecode);

    std::cout << "Factorial(6) result: " << result.ToString() << "\n";
    assert(result.IsSmi());
    assert(result.AsSmi() == 720); // 6! = 720

    std::cout << "  -> Recursive function call and frame dispatch passed successfully.\n";
}

void TestForLoopFibonacci() {
    std::cout << "[Test 4] End-to-End: Parsing & Executing For-Loop Fibonacci...\n";

    std::string_view src =
        "function fib(n) {\n"
        "    var a = 0;\n"
        "    var b = 1;\n"
        "    for (var i = 0; i < n; i = i + 1) {\n"
        "        var next = a + b;\n"
        "        a = b;\n"
        "        b = next;\n"
        "    }\n"
        "    return a;\n"
        "}\n"
        "return fib(10);\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto program = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bytecode = emitter.Emit(program.get());

    Interpreter vm;
    TaggedValue result = vm.Execute(*bytecode);

    std::cout << "Fibonacci(10) result: " << result.ToString() << "\n";
    assert(result.IsSmi());
    assert(result.AsSmi() == 55); // fib(10) = 55

    std::cout << "  -> For loop, variable reassignment, and iterative dispatch passed successfully.\n";
}

void TestObjectLiteralsAndProperties() {
    std::cout << "[Test 5] End-to-End: Parsing & Executing Object Literals & IC Properties...\n";

    std::string_view src =
        "var point = { x: 10, y: 20 };\n"
        "return point.x + point.y;\n";

    Lexer lexer(src);
    Parser parser(lexer);
    auto program = parser.ParseProgram();

    BytecodeEmitter emitter;
    auto bytecode = emitter.Emit(program.get());

    std::cout << bytecode->Disassemble() << "\n";

    Interpreter vm;
    TaggedValue result = vm.Execute(*bytecode);

    std::cout << "point.x + point.y result: " << result.ToString() << "\n";
    assert(result.IsSmi());
    assert(result.AsSmi() == 30);

    std::cout << "  -> Object literals and inline-cached property accesses passed successfully.\n";
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 2 Verification Test Suite          \n"
              << "========================================================\n\n" << std::flush;

    TestLexer();
    std::cout << "\n" << std::flush;
    TestParserAST();
    std::cout << "\n" << std::flush;
    TestRecursiveFactorial();
    std::cout << "\n" << std::flush;
    TestForLoopFibonacci();
    std::cout << "\n" << std::flush;
    TestObjectLiteralsAndProperties();

    std::cout << "\n========================================================\n"
              << "   ALL MILESTONE 2 ARCHITECTURAL TESTS PASSED (5/5)     \n"
              << "========================================================\n" << std::flush;
    return 0;
}
