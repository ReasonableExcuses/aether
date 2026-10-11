#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>

using namespace aether;

void TestNullishCoalescing() {
    std::cout << "[Test 1] Verifying Nullish Coalescing Operator (??)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let t1 = (null ?? 42) === 42;
        let t2 = (undefined ?? 99) === 99;
        let t3 = (0 ?? 100) === 0;
        let t4 = (false ?? true) === false;
        let t5 = ("" ?? "fallback") === "";
        let t6 = (null ?? undefined ?? 55) === 55;
        let t7 = (123 ?? 456) === 123;

        t1 && t2 && t3 && t4 && t5 && t6 && t7;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Nullish coalescing boundary and short-circuit semantics verified.\n" << std::flush;
}

void TestOptionalChaining() {
    std::cout << "[Test 2] Verifying Optional Chaining (?., ?.[...], ?.())...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let objNull = null;
        let r1 = (objNull?.prop) === undefined;

        let obj = { x: 10, y: { z: 42 } };
        let r2 = (obj?.x) === 10;
        let r3 = (obj?.y?.z) === 42;
        let r4 = (objNull?.y?.z) === undefined;

        let arrNull = null;
        let r5 = (arrNull?.[0]) === undefined;

        let arr = [100, 200, 300];
        let r6 = (arr?.[1]) === 200;

        let fnUndef = undefined;
        let r7 = (fnUndef?.()) === undefined;

        let add = (a, b) => a + b;
        let r8 = (add?.(3, 4)) === 7;

        r1 && r2 && r3 && r4 && r5 && r6 && r7 && r8;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Optional property, index, and call chaining verified.\n" << std::flush;
}

void TestLogicalAssignments() {
    std::cout << "[Test 3] Verifying Logical Assignment Operators (&&=, ||=, ??=)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // ||= (or-assign)
        let x1 = 0;
        x1 ||= 10;
        let r1 = (x1 === 10);

        let x2 = 5;
        x2 ||= 10;
        let r2 = (x2 === 5);

        // &&= (and-assign)
        let y1 = 1;
        y1 &&= 20;
        let r3 = (y1 === 20);

        let y2 = 0;
        y2 &&= 20;
        let r4 = (y2 === 0);

        // ??= (nullish-assign)
        let z1 = null;
        z1 ??= 30;
        let r5 = (z1 === 30);

        let z2 = 0;
        z2 ??= 30;
        let r6 = (z2 === 0);

        let z3 = undefined;
        z3 ??= 50;
        let r7 = (z3 === 50);

        r1 && r2 && r3 && r4 && r5 && r6 && r7;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Logical assignment desugaring and assignment semantics verified.\n" << std::flush;
}

void TestTemplateLiterals() {
    std::cout << "[Test 4] Verifying Template Literals (`...` with embedded ${...})...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let s1 = `hello world`;
        let r1 = (s1 === "hello world");

        let name = "Aether";
        let s2 = `Hello ${name}!`;
        let r2 = (s2 === "Hello Aether!");

        let a = 10;
        let b = 20;
        let s3 = `sum: ${a + b}`;
        let r3 = (s3 === "sum: 30");

        let s4 = `values: ${1}, ${2}, ${3}`;
        let r4 = (s4 === "values: 1, 2, 3");

        r1 && r2 && r3 && r4;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Template literal quasis and expression interpolation verified.\n" << std::flush;
}

void TestRestParametersAndArraySpread() {
    std::cout << "[Test 5] Verifying Rest Parameters (...rest) and Array Spread ([...arr])...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function sumAll(first, ...rest) {
            let total = first;
            for (let i = 0; i < rest.length; i++) {
                total += rest[i];
            }
            return total;
        }

        let r1 = (sumAll(10, 1, 2, 3) === 16);
        let r2 = (sumAll(5) === 5);

        let a = [1, 2];
        let b = [...a, 3, 4];
        let r3 = (b.length === 4 && b[0] === 1 && b[1] === 2 && b[2] === 3 && b[3] === 4);

        let c = [0, ...b, 5];
        let r4 = (c.length === 6 && c[0] === 0 && c[1] === 1 && c[4] === 4 && c[5] === 5);

        r1 && r2 && r3 && r4;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Rest parameter packaging and array spreading verified.\n" << std::flush;
}

void TestObjectSpread() {
    std::cout << "[Test 6] Verifying Object Spread ({ ...obj, prop: val })...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let orig = { a: 1, b: 2 };
        let copy = { ...orig, c: 3 };
        let r1 = (copy.a === 1 && copy.b === 2 && copy.c === 3);

        let overridden = { ...orig, b: 20 };
        let r2 = (overridden.a === 1 && overridden.b === 20);

        let extra = { d: 4 };
        let merged = { ...orig, ...extra };
        let r3 = (merged.a === 1 && merged.b === 2 && merged.d === 4);

        r1 && r2 && r3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Object property spreading and overwrite semantics verified.\n" << std::flush;
}

void TestDestructuring() {
    std::cout << "[Test 7] Verifying Array and Object Destructuring...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // Array destructuring
        let [x, y] = [10, 20];
        let r1 = (x === 10 && y === 20);

        // Object destructuring
        let { a, b } = { a: 100, b: 200 };
        let r2 = (a === 100 && b === 200);

        // Object destructuring with aliases
        let { u: aliasU, v: aliasV } = { u: 1, v: 2 };
        let r3 = (aliasU === 1 && aliasV === 2);

        r1 && r2 && r3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Array and object destructuring desugaring verified.\n" << std::flush;
}

void TestArrowFunctions() {
    std::cout << "[Test 8] Verifying Arrow Functions (x => ..., (a, b) => ..., () => ...)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let doubleNum = x => x * 2;
        let r1 = (doubleNum(21) === 42);

        let add = (a, b) => a + b;
        let r2 = (add(15, 25) === 40);

        let getAnswer = () => 42;
        let r3 = (getAnswer() === 42);

        let blockArrow = (a, b) => {
            let temp = a + b;
            return temp * 3;
        };
        let r4 = (blockArrow(2, 3) === 15);

        let restArrow = (...items) => items.length;
        let r5 = (restArrow(1, 2, 3, 4) === 4);

        r1 && r2 && r3 && r4 && r5;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Arrow function variants, expression bodies, and blocks verified.\n" << std::flush;
}

void TestEndToEndModernECMAScriptScript() {
    std::cout << "[Test 9] End-to-End Modern ECMAScript Execution...\n" << std::flush;

    Agent agent;

    std::string script = R"raw(
        // Combine modern syntax features into an idiomatic workflow
        let createItem = (id, name, ...tags) => {
            return {
                id: id,
                name: name,
                tags: [...tags]
            };
        };

        let item1 = createItem(1, "engine", "fast", "jit");
        let item2 = createItem(2, "parser");

        let { name: n1, tags: t1 } = item1;
        let { name: n2, tags: t2 } = item2;

        let tagCount1 = t1?.length ?? 0;
        let tagCount2 = t2?.length ?? 0;

        let summary = `Item 1: ${n1} (${tagCount1} tags)`;
        let expectedSummary = "Item 1: engine (2 tags)";

        let updatedItem1 = { ...item1, verified: true };
        let isVerified = updatedItem1?.verified ?? false;

        let pass = (n1 === "engine") && 
                   (tagCount1 === 2) && 
                   (tagCount2 === 0) && 
                   (summary === expectedSummary) && 
                   isVerified;
        pass;
    )raw";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> End-to-end modern ECMAScript application execution verified.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone 12: Modern ECMAScript Syntax & Lowerings\n";
    std::cout << "=========================================================\n\n";

    TestNullishCoalescing();
    TestOptionalChaining();
    TestLogicalAssignments();
    TestTemplateLiterals();
    TestRestParametersAndArraySpread();
    TestObjectSpread();
    TestDestructuring();
    TestArrowFunctions();
    TestEndToEndModernECMAScriptScript();

    std::cout << "\n[PASS] All 9 Milestone 12 Modern Syntax Verification Suites Passed!\n";
    return 0;
}
