#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"
#include "aether/module.h"
#include "aether/event_loop.h"

#include <iostream>
#ifdef _WIN32
#include <crtdbg.h>
#endif

using namespace aether;

void Test1_BasicNamedImportExport() {
    std::cout << "[Test 1] Verifying basic named import and export...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("math.mjs", R"(
        export function add(a, b) {
            return a + b;
        }
        export function mul(a, b) {
            return a * b;
        }
        export const PI = 3;
    )");

    std::string main_code = R"(
        import { add, mul, PI } from "math.mjs";
        let sum = add(10, 20);
        let prod = mul(sum, PI);
        prod;
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    assert(result.IsSmi() && result.AsSmi() == 90);
    std::cout << "  -> Basic named import and export verified.\n" << std::flush;
}

void Test2_DefaultImportExport() {
    std::cout << "[Test 2] Verifying default import and export (functions & expressions)...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("greeter.mjs", R"(
        export default function greet(name) {
            return "Hello, " + name + "!";
        }
    )");

    loader.RegisterVirtualModule("answer.mjs", R"(
        export default 42;
    )");

    std::string main_code = R"(
        import greet from "greeter.mjs";
        import answer from "answer.mjs";

        let msg = greet("Aether");
        let ans = answer;
        (msg === "Hello, Aether!") && (ans === 42);
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Default import and export verified.\n" << std::flush;
}

void Test3_NamespaceImport() {
    std::cout << "[Test 3] Verifying namespace import (import * as ns) and [Symbol.toStringTag]...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("geometry.mjs", R"(
        export function area(w, h) {
            return w * h;
        }
        export const kind = "rectangle";
    )");

    std::string main_code = R"(
        import * as geo from "geometry.mjs";

        let a = geo.area(5, 6);
        let k = geo.kind;
        let tag = geo[Symbol.toStringTag];

        (a === 30) && (k === "rectangle") && (tag === "Module");
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    if (!result.IsBoolean() || !result.AsBoolean()) {
        std::cerr << "  [Diagnostic] Test 3 failed, result = " << result.ToString() << "\n";
    }
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Namespace import and Symbol.toStringTag verified.\n" << std::flush;
}

void Test4_LiveBindings() {
    std::cout << "[Test 4] Verifying live bindings on mutated exports...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("counter.mjs", R"(
        let count = 0;
        export function increment() {
            count = count + 1;
        }
        export function getInternalCount() {
            return count;
        }
        export { count };
    )");

    std::string main_code = R"(
        import { count, increment, getInternalCount } from "counter.mjs";

        let initial = count;
        increment();
        let afterFirst = count;
        increment();
        let afterSecond = count;
        let internal = getInternalCount();

        (initial === 0) && (afterFirst === 1) && (afterSecond === 2) && (internal === 2);
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Live bindings across module boundaries verified.\n" << std::flush;
}

void Test5_ReadOnlyImportedBindings() {
    std::cout << "[Test 5] Verifying imported bindings are read-only (TypeError on mutation)...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("constants.mjs", R"(
        export let speed = 100;
    )");

    std::string bad_code = R"(
        import { speed } from "constants.mjs";
        speed = 200;
    )";

    bool caught_error = false;
    try {
        agent.EvaluateModule(bad_code, "main.mjs");
    } catch (const std::exception& e) {
        caught_error = true;
        std::string msg = e.what();
        assert(msg.find("TypeError") != std::string::npos || msg.find("imported binding") != std::string::npos);
    }
    assert(caught_error);
    std::cout << "  -> Read-only imported bindings throwing TypeError verified.\n" << std::flush;
}

void Test6_CircularDependencyResolution() {
    std::cout << "[Test 6] Verifying circular module dependency graph resolution...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("even.mjs", R"(
        import { isOdd } from "odd.mjs";
        export function isEven(n) {
            if (n === 0) return true;
            return isOdd(n - 1);
        }
    )");

    loader.RegisterVirtualModule("odd.mjs", R"(
        import { isEven } from "even.mjs";
        export function isOdd(n) {
            if (n === 0) return false;
            return isEven(n - 1);
        }
    )");

    std::string main_code = R"(
        import { isEven } from "even.mjs";
        import { isOdd } from "odd.mjs";

        let e4 = isEven(4);
        let o5 = isOdd(5);
        let e3 = isEven(3);
        let o6 = isOdd(6);

        e4 && o5 && !e3 && !o6;
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Mutual circular module dependencies resolved correctly.\n" << std::flush;
}

void Test7_ReExports() {
    std::cout << "[Test 7] Verifying re-export patterns (named, star, and star-as)...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("origin.mjs", R"(
        export const x = 10;
        export const y = 20;
        export const z = 30;
    )");

    loader.RegisterVirtualModule("hub.mjs", R"(
        export { x as originX } from "origin.mjs";
        export * from "origin.mjs";
        export * as allOrigin from "origin.mjs";
    )");

    std::string main_code = R"(
        import { originX, y, z, allOrigin } from "hub.mjs";

        (originX === 10) && (y === 20) && (z === 30) && (allOrigin.x === 10) && (allOrigin.y === 20);
    )";

    TaggedValue result = agent.EvaluateModule(main_code, "main.mjs");
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Named, star, and star-as re-exports verified.\n" << std::flush;
}

void Test8_TopLevelAwait() {
    std::cout << "[Test 8] Verifying Top-Level Await (TLA) execution...\n" << std::flush;

    Agent agent;

    std::string tla_code = R"(
        let val = await Promise.resolve(42);
        val * 2;
    )";

    TaggedValue result = agent.EvaluateModuleWithEventLoop(tla_code, "tla_main.mjs");
    if (result.IsHeapObject() && result.AsHeapObject()->IsPromise()) {
        auto* prom = static_cast<JSPromise*>(result.AsHeapObject());
        assert(prom->State() == PromiseState::Fulfilled);
        assert(prom->Result().IsSmi() && prom->Result().AsSmi() == 84);
    } else {
        assert(result.IsSmi() && result.AsSmi() == 84);
    }

    std::cout << "  -> Top-Level Await with promise microtask settlement verified.\n" << std::flush;
}

void Test9_DynamicImport() {
    std::cout << "[Test 9] Verifying dynamic import(specifier) returning Promise...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    loader.RegisterVirtualModule("dynamic_target.mjs", R"(
        export function doubleVal(x) {
            return x * 2;
        }
        export const constantVal = 100;
    )");

    // Test 1: Direct promise inspection
    std::string script1 = R"(
        import("dynamic_target.mjs");
    )";
    TaggedValue p_val = agent.EvaluateScriptWithEventLoop(script1);
    assert(p_val.IsHeapObject() && p_val.AsHeapObject()->IsPromise());
    auto* p = static_cast<JSPromise*>(p_val.AsHeapObject());
    assert(p->State() == PromiseState::Fulfilled);
    assert(p->Result().IsHeapObject() && p->Result().AsHeapObject()->IsJSObject());
    auto* ns = static_cast<JSObject*>(p->Result().AsHeapObject());
    assert(ns->GetNamedProperty("constantVal").IsSmi() && ns->GetNamedProperty("constantVal").AsSmi() == 100);

    // Test 2: In async function with await
    std::string script2 = R"(
        async function run() {
            let mod = await import("dynamic_target.mjs");
            return mod.doubleVal(mod.constantVal);
        }
        run();
    )";
    TaggedValue res = agent.EvaluateScriptWithEventLoop(script2);
    assert(res.IsHeapObject() && res.AsHeapObject()->IsPromise());
    auto* res_p = static_cast<JSPromise*>(res.AsHeapObject());
    assert(res_p->State() == PromiseState::Fulfilled);
    assert(res_p->Result().IsSmi() && res_p->Result().AsSmi() == 200);

    std::cout << "  -> Dynamic import() promise settlement to namespace verified.\n" << std::flush;
}

void Test10_CommonJSInterop() {
    std::cout << "[Test 10] Verifying CommonJS <-> ESM bidirectional interoperability...\n" << std::flush;

    Agent agent;
    auto& loader = agent.GetModuleLoader();

    // 1. ESM importing CommonJS
    loader.RegisterVirtualModule("legacy_addon.cjs", R"(
        module.exports = {
            version: 1,
            compute: function(a, b) {
                return a + b;
            }
        };
    )");

    std::string esm_code = R"(
        import legacy from "legacy_addon.cjs";
        import { version, compute } from "legacy_addon.cjs";

        let r1 = legacy.compute(10, 20);
        let r2 = compute(5, 5);
        (r1 === 30) && (r2 === 10) && (version === 1) && (legacy.version === 1);
    )";

    TaggedValue res_esm = agent.EvaluateModule(esm_code, "test_esm.mjs");
    assert(res_esm.IsBoolean() && res_esm.AsBoolean());

    // 2. CommonJS requiring ESM
    loader.RegisterVirtualModule("modern_lib.mjs", R"(
        export function multiply(a, b) {
            return a * b;
        }
        export const release = 2026;
    )");

    std::string cjs_code = R"(
        const modern = require("modern_lib.mjs");
        let p = modern.multiply(4, 5);
        let r = modern.release;
        (p === 20) && (r === 2026);
    )";

    TaggedValue res_cjs = agent.EvaluateScript(cjs_code);
    assert(res_cjs.IsBoolean() && res_cjs.AsBoolean());

    std::cout << "  -> Bidirectional CommonJS <-> ESM interop verified.\n" << std::flush;
}

int main() {
#ifdef _WIN32
    _CrtSetReportMode(_CRT_ASSERT, _CRTDBG_MODE_FILE);
    _CrtSetReportFile(_CRT_ASSERT, _CRTDBG_FILE_STDERR);
#endif
    std::cout << "=================================================================\n";
    std::cout << "Running Aether Milestone 17 Test Suite: ECMAScript Modules (ESM)\n";
    std::cout << "=================================================================\n" << std::flush;

    Test1_BasicNamedImportExport();
    Test2_DefaultImportExport();
    Test3_NamespaceImport();
    Test4_LiveBindings();
    Test5_ReadOnlyImportedBindings();
    Test6_CircularDependencyResolution();
    Test7_ReExports();
    Test8_TopLevelAwait();
    Test9_DynamicImport();
    Test10_CommonJSInterop();

    std::cout << "=================================================================\n";
    std::cout << "All Milestone 17 ECMAScript Module Tests Passed (10/10)!\n";
    std::cout << "=================================================================\n" << std::flush;
    return 0;
}
