#include "aether/common.h"
#include "aether/value.h"
#include "aether/runtime.h"
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

static TaggedValue RunScript(std::string_view source) {
    try {
        Lexer lexer(source);
        Parser parser(lexer);
        auto program = parser.ParseProgram();

        BytecodeEmitter emitter;
        auto bytecode = emitter.Emit(program.get());

        Interpreter interpreter;
        return interpreter.Execute(*bytecode);
    } catch (const std::exception& e) {
        std::cerr << "EXCEPTION in RunScript: " << e.what() << "\n" << std::flush;
        return TaggedValue::Undefined();
    }
}

void TestLexicalClosures() {
    std::cout << "[Test 1] Verifying Lexical Scopes and Closures...\n" << std::flush;

    // 1. Basic Counter Closure
    const char* src1 = 
        "function makeCounter(init) {\n"
        "    var count = init;\n"
        "    return function() {\n"
        "        count = count + 1;\n"
        "        return count;\n"
        "    };\n"
        "}\n"
        "var c1 = makeCounter(10);\n"
        "var c2 = makeCounter(100);\n"
        "var r1 = c1();\n"
        "var r2 = c1();\n"
        "var r3 = c2();\n"
        "var r4 = c1();\n"
        "r1 + r2 + r3 + r4;\n";

    TaggedValue res1 = RunScript(src1);
    // r1 = 11, r2 = 12, r3 = 101, r4 = 13 => 11 + 12 + 101 + 13 = 137
    assert(res1.IsSmi());
    assert(res1.AsSmi() == 137);

    // 2. Multi-level Nested Closures
    const char* src2 =
        "function add(x) {\n"
        "    return function(y) {\n"
        "        return function(z) {\n"
        "            return x + y + z;\n"
        "        };\n"
        "    };\n"
        "}\n"
        "var f1 = add(10);\n"
        "var f2 = f1(20);\n"
        "f2(30);\n";

    TaggedValue res2 = RunScript(src2);
    // 10 + 20 + 30 = 60
    assert(res2.IsSmi());
    assert(res2.AsSmi() == 60);

    std::cout << "  -> Lexical closures and multi-level environment chains verified successfully.\n" << std::flush;
}

void TestPrototypeInheritance() {
    std::cout << "[Test 2] Verifying Prototype Chain Resolution and Shadowing...\n" << std::flush;

    Interpreter interpreter;

    // 1. Direct Prototype Linking
    auto* proto = new JSObject(interpreter.GetRootShape());
    uint32_t shape_id = 1;
    proto->SetNamedProperty("baseVal", TaggedValue::FromSmi(42), shape_id);

    auto* child = new JSObject(interpreter.GetRootShape());
    child->SetPrototype(proto);
    child->SetNamedProperty("childVal", TaggedValue::FromSmi(10), shape_id);

    // Lookup on child directly
    TaggedValue v_child = child->GetNamedProperty("childVal");
    assert(v_child.IsSmi() && v_child.AsSmi() == 10);

    // Lookup delegated to prototype
    TaggedValue v_proto = child->GetNamedProperty("baseVal");
    assert(v_proto.IsSmi() && v_proto.AsSmi() == 42);

    // Shadowing: write to child, verify parent remains unchanged
    child->SetNamedProperty("baseVal", TaggedValue::FromSmi(99), shape_id);
    assert(child->GetNamedProperty("baseVal").AsSmi() == 99);
    assert(proto->GetNamedProperty("baseVal").AsSmi() == 42);

    // 2. Constructor Prototype Chain via JavaScript code
    const char* src =
        "function Animal(nameVal) {\n"
        "    this.nameVal = nameVal;\n"
        "}\n"
        "Animal.prototype.getVal = function() {\n"
        "    return this.nameVal;\n"
        "};\n"
        "var a = new Animal(77);\n"
        "a.getVal();\n";

    TaggedValue res = RunScript(src);
    assert(res.IsSmi());
    assert(res.AsSmi() == 77);

    std::cout << "  -> Prototype delegation, shadowing, and constructor prototypes verified.\n" << std::flush;
}

void TestExceptionHandling() {
    std::cout << "[Test 3] Verifying Exception Handling (try, catch, finally, throw)...\n" << std::flush;

    // 1. Direct try/catch
    const char* src1 =
        "var result = 0;\n"
        "try {\n"
        "    throw 42;\n"
        "    result = 999;\n"
        "} catch (err) {\n"
        "    result = err + 8;\n"
        "}\n"
        "result;\n";

    TaggedValue res1 = RunScript(src1);
    assert(res1.IsSmi());
    assert(res1.AsSmi() == 50);

    // 2. Unwinding across function calls
    const char* src2 =
        "function fail() {\n"
        "    throw 25;\n"
        "}\n"
        "function helper() {\n"
        "    fail();\n"
        "    return 100;\n"
        "}\n"
        "var val = 0;\n"
        "try {\n"
        "    val = helper();\n"
        "} catch (e) {\n"
        "    val = e * 4;\n"
        "}\n"
        "val;\n";

    TaggedValue res2 = RunScript(src2);
    assert(res2.IsSmi());
    assert(res2.AsSmi() == 100);

    // 3. Try-Catch-Finally
    const char* src3 =
        "var status = 0;\n"
        "try {\n"
        "    throw 5;\n"
        "} catch (e) {\n"
        "    status = status + 10;\n"
        "} finally {\n"
        "    status = status + 20;\n"
        "}\n"
        "status;\n";

    TaggedValue res3 = RunScript(src3);
    assert(res3.IsSmi());
    assert(res3.AsSmi() == 30);

    std::cout << "  -> Try-catch stack unwinding and finally semantics verified.\n" << std::flush;
}

void TestClassDeclarationsAndMethods() {
    std::cout << "[Test 4] Verifying ES6 Classes, Constructors, and Inheritance...\n" << std::flush;

    // 1. Class instantiation and method invocation
    const char* src1 =
        "class Point {\n"
        "    constructor(x, y) {\n"
        "        this.x = x;\n"
        "        this.y = y;\n"
        "    }\n"
        "    sum() {\n"
        "        return this.x + this.y;\n"
        "    }\n"
        "}\n"
        "var pt = new Point(15, 25);\n"
        "pt.sum();\n";

    TaggedValue res1 = RunScript(src1);
    assert(res1.IsSmi());
    assert(res1.AsSmi() == 40);

    // 2. Class Inheritance with 'extends'
    const char* src2 =
        "class Base {\n"
        "    constructor(a) {\n"
        "        this.a = a;\n"
        "    }\n"
        "    getBase() {\n"
        "        return this.a;\n"
        "    }\n"
        "}\n"
        "class Derived extends Base {\n"
        "    constructor(a, b) {\n"
        "        this.a = a;\n"
        "        this.b = b;\n"
        "    }\n"
        "    getDerived() {\n"
        "        return this.a + this.b;\n"
        "    }\n"
        "}\n"
        "var d = new Derived(100, 50);\n"
        "var basePart = d.getBase();\n"
        "var derivedPart = d.getDerived();\n"
        "basePart + derivedPart;\n";

    TaggedValue res2 = RunScript(src2);
    // basePart = 100, derivedPart = 150 => 100 + 150 = 250
    assert(res2.IsSmi());
    assert(res2.AsSmi() == 250);

    std::cout << "  -> ES6 classes, constructor allocation, and inherited method dispatch verified.\n" << std::flush;
}

int main() {
    std::cout << "========================================================\n";
    std::cout << "   AetherVM Milestone 7 (Object Model & Closures) Tests \n";
    std::cout << "========================================================\n\n";

    TestLexicalClosures();
    TestPrototypeInheritance();
    TestExceptionHandling();
    TestClassDeclarationsAndMethods();

    std::cout << "\n========================================================\n";
    std::cout << "   ALL MILESTONE 7 TESTS PASSED (4/4)                   \n";
    std::cout << "========================================================\n";
    return 0;
}
