#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>

using namespace aether;

void TestAsyncFunctionReturn() {
    std::cout << "[Test 1] Verifying async function return value and JSPromise wrapper...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function compute(a, b) {
            return a + b;
        }

        let p = compute(15, 27);
        let out = 0;
        p.then(function(val) {
            out = val;
        });
        out;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 42);

    // Verify low-level JSPromise instance directly
    TaggedValue p_val = agent.EvaluateScript("async function greet() { return 'hello async'; } greet();");
    assert(p_val.IsHeapObject() && p_val.AsHeapObject()->IsPromise());
    auto* p = static_cast<JSPromise*>(p_val.AsHeapObject());
    assert(p->State() == PromiseState::Fulfilled);
    assert(p->Result().IsHeapObject() && p->Result().ToString() == "hello async");

    std::cout << "  -> Async function return fulfillment verified.\n" << std::flush;
}

void TestAsyncFunctionAwaitValue() {
    std::cout << "[Test 2] Verifying await with primitive values and microtask coroutine resumption...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function step() {
            let x = await 50;
            let y = await 25;
            return x - y;
        }

        let out = 0;
        step().then(function(val) {
            out = val;
        });
        out;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 25);
    std::cout << "  -> Await on primitive values verified.\n" << std::flush;
}

void TestAsyncFunctionAwaitPromise() {
    std::cout << "[Test 3] Verifying await on resolved Promise instances...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function getAsyncData() {
            let p1 = Promise.resolve(100);
            let val = await p1;
            return val * 2;
        }

        let out = 0;
        getAsyncData().then(function(val) {
            out = val;
        });
        out;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 200);
    std::cout << "  -> Await on Promise instances verified.\n" << std::flush;
}

void TestAsyncFunctionAwaitMultiSequential() {
    std::cout << "[Test 4] Verifying multiple sequential await operations across checkpoints...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function multi() {
            let a = await Promise.resolve(10);
            let b = await Promise.resolve(20);
            let c = await 30;
            let d = await Promise.resolve(40);
            return a + b + c + d;
        }

        let out = 0;
        multi().then(function(val) {
            out = val;
        });
        out;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 100);
    std::cout << "  -> Sequential await coroutine resumption verified.\n" << std::flush;
}

void TestAsyncArrowFunctions() {
    std::cout << "[Test 5] Verifying async arrow functions with block and expression bodies...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let arrowBlock = async (x, y) => {
            let a = await x;
            let b = await y;
            return a + b;
        };

        let arrowExpr = async x => (await x) * 3;

        let total = 0;

        arrowBlock(12, 18).then(function(v1) {
            arrowExpr(15).then(function(v2) {
                total = v1 + v2;
            });
        });

        total;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 75);

    // Verify low-level promise state for async arrow functions
    TaggedValue p1_val = agent.EvaluateScript("let a1 = async (x, y) => { return (await x) + (await y); }; a1(10, 20);");
    assert(p1_val.IsHeapObject() && p1_val.AsHeapObject()->IsPromise());
    auto* p1 = static_cast<JSPromise*>(p1_val.AsHeapObject());
    assert(p1->State() == PromiseState::Fulfilled);
    assert(p1->Result().IsSmi() && p1->Result().AsSmi() == 30);

    TaggedValue p2_val = agent.EvaluateScript("let a2 = async x => (await x) * 4; a2(25);");
    assert(p2_val.IsHeapObject() && p2_val.AsHeapObject()->IsPromise());
    auto* p2 = static_cast<JSPromise*>(p2_val.AsHeapObject());
    assert(p2->State() == PromiseState::Fulfilled);
    assert(p2->Result().IsSmi() && p2->Result().AsSmi() == 100);

    std::cout << "  -> Async arrow functions verified.\n" << std::flush;
}

void TestAsyncFunctionExceptionRejection() {
    std::cout << "[Test 6] Verifying async function uncaught exceptions convert to rejected promises...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function thrower() {
            throw "async failure message";
        }

        let caught = "";
        thrower().catch(function(err) {
            caught = err;
        });
        caught;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsHeapObject() && result.ToString() == "async failure message");

    // Also verify directly on JSPromise instance
    TaggedValue p_val = agent.EvaluateScript("async function boom() { throw 404; } boom();");
    assert(p_val.IsHeapObject() && p_val.AsHeapObject()->IsPromise());
    auto* p = static_cast<JSPromise*>(p_val.AsHeapObject());
    assert(p->State() == PromiseState::Rejected);
    assert(p->Result().IsSmi() && p->Result().AsSmi() == 404);

    std::cout << "  -> Async exception promise rejection verified.\n" << std::flush;
}

void TestAsyncFunctionTryCatchAcrossAwait() {
    std::cout << "[Test 7] Verifying try-catch handler preservation across suspended await points...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function recoverFromRejection() {
            let result = "initial";
            try {
                let rejected = Promise.reject("intentional error");
                await rejected;
                result = "unreached";
            } catch (e) {
                result = "caught: " + e;
            }
            return result;
        }

        let out = "";
        recoverFromRejection().then(function(val) {
            out = val;
        });
        out;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsHeapObject() && result.ToString() == "caught: intentional error");
    std::cout << "  -> Try-catch exception handler preservation across await verified.\n" << std::flush;
}

void TestPromiseRaceAndAllSettled() {
    std::cout << "[Test 8] Verifying Promise.race and Promise.allSettled combinators...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let raceResult = "";
        let settledOk = false;
        let test8_passed = false;

        let p1 = Promise.resolve("winner");
        let p2 = Promise.resolve("loser");
        Promise.race([p1, p2]).then(function(v) {
            raceResult = v;
        });

        let s1 = Promise.resolve(42);
        let s2 = Promise.reject("bad");
        Promise.allSettled([s1, s2]).then(function(arr) {
            let item0 = arr[0];
            let item1 = arr[1];
            if (arr.length === 2 &&
                item0.status === "fulfilled" && item0.value === 42 &&
                item1.status === "rejected" && item1.reason === "bad") {
                settledOk = true;
            }
            if (raceResult === "winner" && settledOk) {
                test8_passed = true;
            }
        });

        test8_passed;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Promise.race and Promise.allSettled verified.\n" << std::flush;
}

void TestPromiseCombinatorAny() {
    std::cout << "[Test 9] Verifying Promise.any combinator and AggregateError rejection...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let anyWinner = "";
        let anyFailed = false;
        let test9_passed = false;

        let r1 = Promise.reject("err1");
        let r2 = Promise.resolve("first-winner");
        let r3 = Promise.reject("err2");

        Promise.any([r1, r2, r3]).then(function(v) {
            anyWinner = v;
        });

        let allBad1 = Promise.reject("fail_a");
        let allBad2 = Promise.reject("fail_b");
        Promise.any([allBad1, allBad2]).catch(function(err) {
            if (err.name === "AggregateError" && err.errors.length === 2 &&
                err.errors[0] === "fail_a" && err.errors[1] === "fail_b") {
                anyFailed = true;
            }
            if (anyWinner === "first-winner" && anyFailed) {
                test9_passed = true;
            }
        });

        test9_passed;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Promise.any fulfillment and AggregateError rejection verified.\n" << std::flush;
}

void TestPromiseFinallyAndIdempotentResolve() {
    std::cout << "[Test 10] Verifying Promise.prototype.finally and idempotent Promise.resolve...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let pOriginal = Promise.resolve(88);
        let pAgain = Promise.resolve(pOriginal);
        let isIdempotent = (pOriginal === pAgain);

        let finallyRanFulfill = false;
        let fulfilledVal = 0;
        let finallyRanReject = false;
        let rejectedReason = "";
        let test10_passed = false;

        Promise.resolve(55).finally(function() {
            finallyRanFulfill = true;
        }).then(function(v) {
            fulfilledVal = v;
        });

        Promise.reject("reason123").finally(function() {
            finallyRanReject = true;
        }).catch(function(err) {
            rejectedReason = err;
            if (isIdempotent && finallyRanFulfill && fulfilledVal === 55 && finallyRanReject && rejectedReason === "reason123") {
                test10_passed = true;
            }
        });

        test10_passed;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Promise.prototype.finally and idempotent Promise.resolve verified.\n" << std::flush;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << " Running AetherVM Milestone 14: Async / Await & Promise Suite   \n";
    std::cout << "=================================================================\n" << std::flush;

    TestAsyncFunctionReturn();
    TestAsyncFunctionAwaitValue();
    TestAsyncFunctionAwaitPromise();
    TestAsyncFunctionAwaitMultiSequential();
    TestAsyncArrowFunctions();
    TestAsyncFunctionExceptionRejection();
    TestAsyncFunctionTryCatchAcrossAwait();
    TestPromiseRaceAndAllSettled();
    TestPromiseCombinatorAny();
    TestPromiseFinallyAndIdempotentResolve();

    std::cout << "=================================================================\n";
    std::cout << " All 10 Milestone 14 Async/Await Tests Passed Successfully!      \n";
    std::cout << "=================================================================\n" << std::flush;

    return 0;
}
