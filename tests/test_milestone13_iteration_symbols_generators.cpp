#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>

using namespace aether;

void TestSymbolPrimitives() {
    std::cout << "[Test 1] Verifying Symbol creation, uniqueness, and typeof...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let s1 = Symbol();
        let s2 = Symbol("description");
        let s3 = Symbol("description");

        let t1 = (typeof s1 === "symbol");
        let t2 = (typeof s2 === "symbol");
        let t3 = (s2 !== s3);
        let t4 = (s1 !== s2);

        // Object with symbol keys
        let obj = {};
        obj[s2] = 100;
        obj[s3] = 200;

        let t5 = (obj[s2] === 100);
        let t6 = (obj[s3] === 200);

        t1 && t2 && t3 && t4 && t5 && t6;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Symbol primitives and symbol-keyed object properties verified.\n" << std::flush;
}

void TestSymbolRegistry() {
    std::cout << "[Test 2] Verifying Global Symbol Registry (Symbol.for, Symbol.keyFor)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let symA = Symbol.for("app.token");
        let symB = Symbol.for("app.token");
        let symLocal = Symbol("app.token");

        let r1 = (symA === symB);
        let r2 = (symA !== symLocal);
        let r3 = (Symbol.keyFor(symA) === "app.token");
        let r4 = (Symbol.keyFor(symLocal) === undefined);

        r1 && r2 && r3 && r4;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Symbol.for and Symbol.keyFor global registry verified.\n" << std::flush;
}

void TestWellKnownSymbols() {
    std::cout << "[Test 3] Verifying Well-Known Symbols (Symbol.iterator, Symbol.toStringTag)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let w1 = (typeof Symbol.iterator === "symbol");
        let w2 = (typeof Symbol.toStringTag === "symbol");
        let w3 = (typeof Symbol.hasInstance === "symbol");
        let w4 = (typeof Symbol.toPrimitive === "symbol");

        let obj = {};
        obj[Symbol.iterator] = 42;
        let w5 = (obj[Symbol.iterator] === 42);

        w1 && w2 && w3 && w4 && w5;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Well-known symbols verified.\n" << std::flush;
}

void TestArrayAndStringIterators() {
    std::cout << "[Test 4] Verifying Array and String built-in iterators...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // Array [Symbol.iterator]
        let arr = [10, 20, 30];
        let iter = arr[Symbol.iterator]();
        let s1 = iter.next();
        let s2 = iter.next();
        let s3 = iter.next();
        let s4 = iter.next();

        let a1 = (s1.value === 10 && !s1.done);
        let a2 = (s2.value === 20 && !s2.done);
        let a3 = (s3.value === 30 && !s3.done);
        let a4 = (s4.value === undefined && s4.done);

        // Array entries
        let e_it = arr.entries();
        let e1 = e_it.next();
        let a5 = (e1.value[0] === 0 && e1.value[1] === 10 && !e1.done);

        // String [Symbol.iterator]
        let str = "abc";
        let str_it = str[Symbol.iterator]();
        let c1 = str_it.next();
        let c2 = str_it.next();
        let c3 = str_it.next();
        let c4 = str_it.next();

        let a6 = (c1.value === "a" && !c1.done);
        let a7 = (c2.value === "b" && !c2.done);
        let a8 = (c3.value === "c" && !c3.done);
        let a9 = (c4.value === undefined && c4.done);

        a1 && a2 && a3 && a4 && a5 && a6 && a7 && a8 && a9;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Array and String iteration protocols verified.\n" << std::flush;
}

void TestMapAndSetIterators() {
    std::cout << "[Test 5] Verifying Map and Set built-in iterators...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let m = new Map();
        m.set("k1", 100);
        m.set("k2", 200);

        let m_it = m[Symbol.iterator]();
        let me1 = m_it.next();
        let me2 = m_it.next();
        let me3 = m_it.next();

        let r1 = (me1.value[0] === "k1" && me1.value[1] === 100 && !me1.done);
        let r2 = (me2.value[0] === "k2" && me2.value[1] === 200 && !me2.done);
        let r3 = (me3.value === undefined && me3.done);

        let s = new Set();
        s.add(5);
        s.add(15);

        let s_it = s[Symbol.iterator]();
        let se1 = s_it.next();
        let se2 = s_it.next();
        let se3 = s_it.next();

        let r4 = (se1.value === 5 && !se1.done);
        let r5 = (se2.value === 15 && !se2.done);
        let r6 = (se3.value === undefined && se3.done);

        r1 && r2 && r3 && r4 && r5 && r6;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Map and Set iteration protocols verified.\n" << std::flush;
}

void TestForOfLoops() {
    std::cout << "[Test 6] Verifying For-Of loops over Arrays, Strings, and Maps...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // Array iteration
        let sum = 0;
        for (let item of [1, 2, 3, 4, 5]) {
            sum = sum + item;
        }
        let t1 = (sum === 15);

        // String iteration
        let chars = "";
        for (let ch of "xyz") {
            chars = chars + ch;
        }
        let t2 = (chars === "xyz");

        // Map destructuring iteration
        let map = new Map();
        map.set("a", 10);
        map.set("b", 20);
        let map_sum = 0;
        for (let [key, val] of map) {
            map_sum = map_sum + val;
        }
        let t3 = (map_sum === 30);

        t1 && t2 && t3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> For-of loops and destructuring iteration verified.\n" << std::flush;
}

void TestGeneratorBasics() {
    std::cout << "[Test 7] Verifying Generator Coroutines (function* and yield)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function* simpleGen() {
            yield 1;
            yield 2;
            return 3;
        }

        let g = simpleGen();
        let r1 = g.next();
        let r2 = g.next();
        let r3 = g.next();
        let r4 = g.next();

        let c1 = (r1.value === 1 && !r1.done);
        let c2 = (r2.value === 2 && !r2.done);
        let c3 = (r3.value === 3 && r3.done);
        let c4 = (r4.value === undefined && r4.done);

        c1 && c2 && c3 && c4;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Generator basics (suspension and completion) verified.\n" << std::flush;
}

void TestGeneratorValuePassing() {
    std::cout << "[Test 8] Verifying Generator two-way value communication (next(val))...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function* calcGen() {
            let a = yield 10;
            let b = yield (a + 5);
            return b * 2;
        }

        let g = calcGen();
        let r1 = g.next();       // yields 10
        let r2 = g.next(20);     // a = 20, yields 25
        let r3 = g.next(50);     // b = 50, returns 100

        let c1 = (r1.value === 10 && !r1.done);
        let c2 = (r2.value === 25 && !r2.done);
        let c3 = (r3.value === 100 && r3.done);

        c1 && c2 && c3;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Generator two-way value passing verified.\n" << std::flush;
}

void TestGeneratorWithForOf() {
    std::cout << "[Test 9] Verifying For-Of iteration over Generator Coroutines...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        function* range(start, end) {
            let cur = start;
            while (cur <= end) {
                yield cur;
                cur = cur + 1;
            }
        }

        let total = 0;
        for (let x of range(1, 5)) {
            total = total + x;
        }

        total === 15;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> For-of loop over generator coroutine verified.\n" << std::flush;
}

void TestCustomIterableAndGeneratorReturn() {
    std::cout << "[Test 10] Verifying Custom Iterable with Symbol.iterator and generator.return()...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        // Generator return() early completion
        function* infiniteNumbers() {
            let i = 1;
            while (true) {
                yield i;
                i = i + 1;
            }
        }

        let g = infiniteNumbers();
        let n1 = g.next();
        let n2 = g.next();
        let ret = g.return(999);
        let n3 = g.next();

        let t1 = (n1.value === 1 && !n1.done);
        let t2 = (n2.value === 2 && !n2.done);
        let t3 = (ret.value === 999 && ret.done);
        let t4 = (n3.value === undefined && n3.done);

        // Custom object implementing [Symbol.iterator] via generator
        let myCollection = {
            items: [100, 200, 300],
            [Symbol.iterator]: function*() {
                yield 100;
                yield 200;
                yield 300;
            }
        };

        let sum = 0;
        for (let v of myCollection) {
            sum = sum + v;
        }
        let t5 = (sum === 600);

        t1 && t2 && t3 && t4 && t5;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Custom iterable and generator return semantics verified.\n" << std::flush;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "  AetherVM Milestone 13: Iteration Protocols, Symbols & Generators\n";
    std::cout << "=================================================================\n\n";

    TestSymbolPrimitives();
    TestSymbolRegistry();
    TestWellKnownSymbols();
    TestArrayAndStringIterators();
    TestMapAndSetIterators();
    TestForOfLoops();
    TestGeneratorBasics();
    TestGeneratorValuePassing();
    TestGeneratorWithForOf();
    TestCustomIterableAndGeneratorReturn();

    std::cout << "\n=================================================================\n";
    std::cout << "  All Milestone 13 Tests Passed Successfully (10/10)!\n";
    std::cout << "=================================================================\n";
    return 0;
}
