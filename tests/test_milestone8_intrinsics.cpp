#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"

#include <iostream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestArrayCoreOperations() {
    std::cout << "[Test 1] Verifying Array Object Model & Core Operations...\n" << std::flush;

    Agent agent;

    TaggedValue res1 = agent.EvaluateScript(R"(
        var a = [10, 20, 30];
        a[1];
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 20);

    // 2. Indexed element assignment and length
    TaggedValue res2 = agent.EvaluateScript(R"(
        var a = [1, 2, 3];
        a[0] = 100;
        a[0] + a.length;
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 103);

    // 3. Array push, pop, shift, unshift
    TaggedValue res3 = agent.EvaluateScript(R"(
        var a = [10, 20];
        a.push(30);
        var p = a.pop();
        a.unshift(5);
        var s = a.shift();
        p + s + a.length;
    )");
    assert(res3.IsSmi() && res3.AsSmi() == 37);

    // 4. Array slice, join, indexOf, reverse
    TaggedValue res4 = agent.EvaluateScript(R"(
        var a = [1, 2, 3, 4, 5];
        var s = a.slice(1, 4);
        s.length;
    )");
    assert(res4.IsSmi() && res4.AsSmi() == 3);

    TaggedValue res5 = agent.EvaluateScript(R"(
        var a = ["Aether", "Virtual", "Machine"];
        a.join(" ");
    )");
    assert(res5.IsHeapObject() && res5.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res5.AsHeapObject())->Value() == "Aether Virtual Machine");

    TaggedValue res6 = agent.EvaluateScript(R"(
        var a = [10, 20, 30, 40];
        a.indexOf(30);
    )");
    assert(res6.IsSmi() && res6.AsSmi() == 2);

    TaggedValue res7 = agent.EvaluateScript(R"(
        Array.isArray([1, 2, 3]);
    )");
    assert(res7.IsTrue());

    std::cout << "  -> Array core operations and element access verified.\n" << std::flush;
}

void TestArrayHigherOrderMethods() {
    std::cout << "[Test 2] Verifying Array Higher-Order Methods (map, filter, forEach, reduce)...\n" << std::flush;

    Agent agent;

    // 1. Array.prototype.map
    TaggedValue res1 = agent.EvaluateScript(R"(
        var a = [1, 2, 3, 4];
        var doubled = a.map(function(x) { return x * 2; });
        doubled[0] + doubled[1] + doubled[2] + doubled[3];
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 20); // 2 + 4 + 6 + 8 = 20

    // 2. Array.prototype.filter
    TaggedValue res2 = agent.EvaluateScript(R"(
        var a = [10, 25, 30, 5, 45];
        var filtered = a.filter(function(x) { return x > 20; });
        filtered.length;
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 3); // [25, 30, 45]

    // 3. Array.prototype.reduce
    TaggedValue res3 = agent.EvaluateScript(R"(
        var a = [1, 2, 3, 4, 5];
        var sum = a.reduce(function(acc, x) { return acc + x; }, 0);
        sum;
    )");
    assert(res3.IsSmi() && res3.AsSmi() == 15);

    // 4. Array.prototype.forEach
    TaggedValue res4 = agent.EvaluateScript(R"(
        var a = [2, 4, 6];
        var total = 0;
        a.forEach(function(x) { total = total + x; });
        total;
    )");
    assert(res4.IsSmi() && res4.AsSmi() == 12);

    std::cout << "  -> Higher-order functional array intrinsics verified.\n" << std::flush;
}

void TestStringPrototypeIntrinsics() {
    std::cout << "[Test 3] Verifying String Prototype Methods...\n" << std::flush;

    Agent agent;

    // 1. charAt & charCodeAt
    TaggedValue res1 = agent.EvaluateScript(R"(
        var s = "Aether";
        s.charAt(0);
    )");
    assert(res1.IsHeapObject() && res1.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res1.AsHeapObject())->Value() == "A");

    TaggedValue res2 = agent.EvaluateScript(R"(
        var s = "Aether";
        s.charCodeAt(0);
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 65); // ASCII 'A'

    // 2. toLowerCase & toUpperCase & trim
    TaggedValue res3 = agent.EvaluateScript(R"(
        var s = "  HeLLo WoRLd  ";
        var trimmed = s.trim();
        trimmed.toUpperCase();
    )");
    assert(res3.IsHeapObject() && res3.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res3.AsHeapObject())->Value() == "HELLO WORLD");

    // 3. slice & substring & indexOf
    TaggedValue res4 = agent.EvaluateScript(R"(
        var s = "ECMAScript-2026";
        var sub = s.slice(0, 10);
        var idx = s.indexOf("2026");
        sub.length + idx;
    )");
    assert(res4.IsSmi() && res4.AsSmi() == 21); // 10 + 11 = 21

    // 4. split
    TaggedValue res5 = agent.EvaluateScript(R"(
        var s = "apple,banana,orange";
        var parts = s.split(",");
        parts.length;
    )");
    assert(res5.IsSmi() && res5.AsSmi() == 3);

    TaggedValue res6 = agent.EvaluateScript(R"(
        var s = "apple,banana,orange";
        var parts = s.split(",");
        parts[1];
    )");
    assert(res6.IsHeapObject() && res6.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res6.AsHeapObject())->Value() == "banana");

    std::cout << "  -> String prototype methods verified cleanly.\n" << std::flush;
}

void TestJSONParsingAndSerialization() {
    std::cout << "[Test 4] Verifying JSON Parser & Serializer...\n" << std::flush;

    Agent agent;

    // 1. JSON.parse on simple object
    TaggedValue res1 = agent.EvaluateScript(R"(
        var data = JSON.parse("{\"alpha\": 42, \"beta\": 100}");
        data.alpha + data.beta;
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 142);

    // 2. JSON.parse on array
    TaggedValue res2 = agent.EvaluateScript(R"(
        var arr = JSON.parse("[10, 20, 30, 40]");
        arr[0] + arr[3];
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 50);

    // 3. JSON.stringify on array
    TaggedValue res3 = agent.EvaluateScript(R"(
        var arr = [1, 2, 3];
        JSON.stringify(arr);
    )");
    assert(res3.IsHeapObject() && res3.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res3.AsHeapObject())->Value() == "[1,2,3]");

    // 4. JSON Round-Trip
    TaggedValue res4 = agent.EvaluateScript(R"(
        var original = [10, 20, 30];
        var serialized = JSON.stringify(original);
        var restored = JSON.parse(serialized);
        restored[0] + restored[1] + restored[2];
    )");
    assert(res4.IsSmi() && res4.AsSmi() == 60);

    std::cout << "  -> JSON parser and serializer verified.\n" << std::flush;
}

void TestMapAndSetCollections() {
    std::cout << "[Test 5] Verifying Map and Set Collections...\n" << std::flush;

    Agent agent;

    // 1. Map operations
    TaggedValue res1 = agent.EvaluateScript(R"(
        var m = new Map();
        m.set("key1", 100);
        m.set("key2", 200);
        var v1 = m.get("key1");
        var hasKey = m.has("key2");
        var s = m.size;
        v1 + s;
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 102); // 100 + 2 = 102

    TaggedValue res2 = agent.EvaluateScript(R"(
        var m = new Map();
        m.set("k", 55);
        m.delete("k");
        m.has("k");
    )");
    assert(res2.IsFalse());

    // 2. Set operations
    TaggedValue res3 = agent.EvaluateScript(R"(
        var s = new Set();
        s.add(10);
        s.add(20);
        s.add(10); // Duplicate should be ignored
        s.size;
    )");
    assert(res3.IsSmi() && res3.AsSmi() == 2);

    TaggedValue res4 = agent.EvaluateScript(R"(
        var s = new Set();
        s.add("alpha");
        s.add("beta");
        var hasAlpha = s.has("alpha");
        s.delete("alpha");
        var hasAfter = s.has("alpha");
        hasAlpha && (!hasAfter);
    )");
    assert(res4.IsTrue());

    std::cout << "  -> Map and Set collections verified.\n" << std::flush;
}

void TestPromiseAndMicrotasks() {
    std::cout << "[Test 6] Verifying Promise/A+ and Microtask Execution Chain...\n" << std::flush;

    Agent agent;

    // 1. Direct Promise constructor and resolve
    TaggedValue res1 = agent.EvaluateScript(R"(
        var result = 0;
        var p = new Promise(function(resolve, reject) {
            resolve(42);
        });
        p.then(function(val) {
            result = val;
        });
        result;
    )");
    // After microtask checkpoint at end of script execution, result is updated!
    assert(res1.IsSmi() && res1.AsSmi() == 42);

    // 2. Promise chaining
    TaggedValue res2 = agent.EvaluateScript(R"(
        var finalValue = 0;
        Promise.resolve(10)
            .then(function(x) { return x * 2; })
            .then(function(y) { finalValue = y + 5; });
        finalValue;
    )");
    assert(res2.IsSmi() && res2.AsSmi() == 25);

    // 3. Promise rejection & catch
    TaggedValue res3 = agent.EvaluateScript(R"(
        var caughtError = "";
        Promise.reject("FailedOperation")
            .catch(function(err) {
                caughtError = err;
            });
        caughtError;
    )");
    assert(res3.IsHeapObject() && res3.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res3.AsHeapObject())->Value() == "FailedOperation");

    // 4. Promise.all
    TaggedValue res4 = agent.EvaluateScript(R"(
        var sum = 0;
        var p1 = Promise.resolve(10);
        var p2 = Promise.resolve(20);
        var p3 = Promise.resolve(30);
        Promise.all([p1, p2, p3]).then(function(results) {
            sum = results[0] + results[1] + results[2];
        });
        sum;
    )");
    assert(res4.IsSmi() && res4.AsSmi() == 60);

    // 5. RegExp builtin test
    TaggedValue res5 = agent.EvaluateScript(R"(
        var re = new RegExp("Aether[0-9]+");
        re.test("Aether2026");
    )");
    assert(res5.IsTrue());

    std::cout << "  -> Promise/A+, Microtask Queue, and RegExp verified.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone 8 Test Suite: Standard Library Intrinsics\n";
    std::cout << "=========================================================\n\n";

    TestArrayCoreOperations();
    TestArrayHigherOrderMethods();
    TestStringPrototypeIntrinsics();
    TestJSONParsingAndSerialization();
    TestMapAndSetCollections();
    TestPromiseAndMicrotasks();

    std::cout << "\n[PASS] All 6 Milestone 8 Intrinsics Verification Suites Passed!\n";
    return 0;
}
