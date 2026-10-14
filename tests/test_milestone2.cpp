#include "aether/common.h"
#include "aether/lexer.h"
#include "aether/parser.h"
#include "aether/bytecode_emitter.h"
#include "aether/interpreter.h"

#include <iostream>
#ifdef NDEBUG
#undef NDEBUG
#endif
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

void TestExtendedECMAScriptLexer() {
    std::cout << "[Test 1b] Verifying Complete Extended ECMAScript Tokens & Literals...\n";

    // 1. Hashbang and modern operators
    std::string_view code1 =
        "#!/usr/bin/env node\n"
        "switch (x) { case 1: break; default: continue; }\n"
        "do { void 0; debugger; with(o){} } while(false);\n"
        "class B extends A { static foo() { super.foo(); } }\n"
        "a **= b ** c;\n"
        "x >>>= y >>> z;\n"
        "x <<= 1; x >>= 2; x &= 3; x |= 4; x ^= 5; x %= 6;\n"
        "#privateField;\n";

    Lexer lexer1(code1);
    std::vector<TokenKind> expected_tokens = {
        TokenKind::Switch, TokenKind::OpenParen, TokenKind::Identifier, TokenKind::CloseParen,
        TokenKind::OpenBrace, TokenKind::Case, TokenKind::Number, TokenKind::Colon, TokenKind::Break, TokenKind::Semicolon,
        TokenKind::Default, TokenKind::Colon, TokenKind::Continue, TokenKind::Semicolon, TokenKind::CloseBrace,
        TokenKind::Do, TokenKind::OpenBrace, TokenKind::Void, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Debugger, TokenKind::Semicolon, TokenKind::With, TokenKind::OpenParen, TokenKind::Identifier,
        TokenKind::CloseParen, TokenKind::OpenBrace, TokenKind::CloseBrace, TokenKind::CloseBrace,
        TokenKind::While, TokenKind::OpenParen, TokenKind::False, TokenKind::CloseParen, TokenKind::Semicolon,
        TokenKind::Class, TokenKind::Identifier, TokenKind::Extends, TokenKind::Identifier,
        TokenKind::OpenBrace, TokenKind::Static, TokenKind::Identifier, TokenKind::OpenParen, TokenKind::CloseParen,
        TokenKind::OpenBrace, TokenKind::Super, TokenKind::Dot, TokenKind::Identifier, TokenKind::OpenParen, TokenKind::CloseParen, TokenKind::Semicolon,
        TokenKind::CloseBrace, TokenKind::CloseBrace,
        TokenKind::Identifier, TokenKind::StarStarEqual, TokenKind::Identifier, TokenKind::StarStar, TokenKind::Identifier, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::GreaterGreaterGreaterEqual, TokenKind::Identifier, TokenKind::GreaterGreaterGreater, TokenKind::Identifier, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::LessLessEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::GreaterGreaterEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::AmpersandEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::BarEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::CaretEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Identifier, TokenKind::PercentEqual, TokenKind::Number, TokenKind::Semicolon,
        TokenKind::Hash, TokenKind::Identifier, TokenKind::Semicolon,
        TokenKind::EndOfFile
    };

    for (TokenKind exp : expected_tokens) {
        Token tok = lexer1.NextToken();
        if (tok.kind != exp) {
            std::cerr << "Mismatch: expected " << TokenKindToString(exp) << ", got " << TokenKindToString(tok.kind) << " ('" << tok.text << "')\n";
        }
        assert(tok.kind == exp);
    }

    // 2. Number formats (binary, octal, hex, separators, floats, BigInt)
    std::string_view num_code = "0b1010 0o755 0xFF 1_000_000 0b10_01 0xFF_EE .5 .125e2 42n 0x1Fn";
    Lexer num_lexer(num_code);
    Token t1 = num_lexer.NextToken(); // 0b1010
    assert(t1.kind == TokenKind::Number && t1.int_value == 10 && t1.is_integer);
    Token t2 = num_lexer.NextToken(); // 0o755
    assert(t2.kind == TokenKind::Number && t2.int_value == 493 && t2.is_integer);
    Token t3 = num_lexer.NextToken(); // 0xFF
    assert(t3.kind == TokenKind::Number && t3.int_value == 255 && t3.is_integer);
    Token t4 = num_lexer.NextToken(); // 1_000_000
    assert(t4.kind == TokenKind::Number && t4.int_value == 1000000 && t4.is_integer);
    Token t5 = num_lexer.NextToken(); // 0b10_01
    assert(t5.kind == TokenKind::Number && t5.int_value == 9 && t5.is_integer);
    Token t6 = num_lexer.NextToken(); // 0xFF_EE
    assert(t6.kind == TokenKind::Number && t6.int_value == 0xFFEE && t6.is_integer);
    Token t7 = num_lexer.NextToken(); // .5
    assert(t7.kind == TokenKind::Number && t7.number_value == 0.5 && !t7.is_integer);
    Token t8 = num_lexer.NextToken(); // .125e2
    assert(t8.kind == TokenKind::Number && t8.number_value == 12.5 && !t8.is_integer);
    Token t9 = num_lexer.NextToken(); // 42n
    assert(t9.kind == TokenKind::Number && t9.is_bigint);
    Token t10 = num_lexer.NextToken(); // 0x1Fn
    assert(t10.kind == TokenKind::Number && t10.is_bigint);

    // 3. String escapes (\0, \b, \f, \v, \x41, \u0041, \u{41})
    std::string_view str_code = "\"\\0\\b\\f\\v\\x41\\u0042\\u{43}\"";
    Lexer str_lexer(str_code);
    Token st = str_lexer.NextToken();
    assert(st.kind == TokenKind::String);
    assert(st.text.size() == 7);
    assert(st.text[0] == '\0');
    assert(st.text[1] == '\b');
    assert(st.text[2] == '\f');
    assert(st.text[3] == '\v');
    assert(st.text[4] == 'A');
    assert(st.text[5] == 'B');
    assert(st.text[6] == 'C');

    std::cout << "  -> All extended ECMAScript tokens, literals, escapes, and hashbang comments verified.\n";
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

void TestExtendedECMAScriptParserAndRuntime() {
    std::cout << "[Test 6] Verifying Extended ECMAScript Grammar (do-while, break, continue, switch, void, debugger, **, >>>, compound assign)...\n" << std::flush;

    // 1. do-while loop
    {
        std::cout << "  - Subtest 1: do-while\n" << std::flush;
        const char* code =
            "let a = 0;\n"
            "let count = 0;\n"
            "do {\n"
            "    count += 1;\n"
            "    a += 2;\n"
            "} while (a < 10);\n"
            "return count;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsSmi() && res.AsSmi() == 5);
    }

    // 2. break and continue in for loop
    {
        std::cout << "  - Subtest 2: break/continue\n" << std::flush;
        const char* code =
            "let sum = 0;\n"
            "for (let i = 0; i < 10; i += 1) {\n"
            "    if (i === 3) continue;\n"
            "    if (i === 6) break;\n"
            "    sum += i;\n"
            "}\n"
            "return sum;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsSmi() && res.AsSmi() == 12);
    }

    // 3. switch with case, break, fallthrough, and default
    {
        std::cout << "  - Subtest 3: switch\n" << std::flush;
        const char* code =
            "function testSwitch(val) {\n"
            "    let res = 0;\n"
            "    switch (val) {\n"
            "        case 1:\n"
            "            res = 10;\n"
            "            break;\n"
            "        case 2:\n"
            "        case 3:\n"
            "            res = 30;\n"
            "            break;\n"
            "        default:\n"
            "            res = 99;\n"
            "            break;\n"
            "    }\n"
            "    return res;\n"
            "}\n"
            "return testSwitch(1) + testSwitch(2) + testSwitch(3) + testSwitch(4);\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsSmi() && res.AsSmi() == 169);
    }

    // 4. void operator, comma operator, and debugger statement
    {
        std::cout << "  - Subtest 4: void/comma/debugger\n" << std::flush;
        const char* code =
            "debugger;\n"
            "let x = 10;\n"
            "let y = void (x += 5);\n"
            "let seq = (1, 2, 3);\n"
            "return x + seq;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsSmi() && res.AsSmi() == 18);
    }

    // 5. Exponentiation ** right-associativity: 2 ** 3 ** 2 == 512
    {
        std::cout << "  - Subtest 5: exponentiation\n" << std::flush;
        const char* code = "return 2 ** 3 ** 2;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsNumber() && res.AsNumber() == 512.0);
    }

    // 6. Unsigned right shift >>>
    {
        std::cout << "  - Subtest 6: shift right unsigned\n" << std::flush;
        const char* code =
            "let neg = -1;\n"
            "return neg >>> 0;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsDouble() && res.AsDouble() == 4294967295.0);
    }

    // 7. Compound assignment operators: +=, -=, *=, /=, %=, **=
    {
        std::cout << "  - Subtest 7: compound assign\n" << std::flush;
        const char* code =
            "let v = 10;\n"
            "v += 2;\n"
            "v -= 4;\n"
            "v *= 3;\n"
            "v /= 2;\n"
            "v %= 7;\n"
            "v **= 3;\n"
            "return v;\n";
        Lexer lexer(code);
        Parser parser(lexer);
        auto prog = parser.ParseProgram();
        BytecodeEmitter emitter;
        auto bc = emitter.Emit(prog.get());
        Interpreter vm;
        TaggedValue res = vm.Execute(*bc);
        assert(res.IsNumber() && res.AsNumber() == 125.0);
    }

    std::cout << "  -> Complete extended ECMAScript grammar, AST lowering, opcodes, and runtime execution verified.\n" << std::flush;
}

int main() {
    std::cout << "========================================================\n"
              << "   AetherVM Milestone 2 Verification Test Suite          \n"
              << "========================================================\n\n" << std::flush;

    TestLexer();
    std::cout << "\n" << std::flush;
    TestExtendedECMAScriptLexer();
    std::cout << "\n" << std::flush;
    TestParserAST();
    std::cout << "\n" << std::flush;
    TestRecursiveFactorial();
    std::cout << "\n" << std::flush;
    TestForLoopFibonacci();
    std::cout << "\n" << std::flush;
    TestObjectLiteralsAndProperties();
    std::cout << "\n" << std::flush;
    TestExtendedECMAScriptParserAndRuntime();

    std::cout << "\n========================================================\n"
              << "   ALL MILESTONE 2 ARCHITECTURAL TESTS PASSED (7/7)     \n"
              << "========================================================\n" << std::flush;
    return 0;
}
