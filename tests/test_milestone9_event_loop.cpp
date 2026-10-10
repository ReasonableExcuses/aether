#include "aether/common.h"
#include "aether/value.h"
#include "aether/heap_object.h"
#include "aether/runtime.h"
#include "aether/event_loop.h"

#include <iostream>
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <cassert>

using namespace aether;

void TestBasicTimersAndArguments() {
    std::cout << "[Test 1] Verifying setTimeout, clearTimeout, and Argument Forwarding...\n" << std::flush;

    Agent agent;

    // 1. setTimeout argument forwarding and execution
    agent.EvaluateScript(R"(
        globalThis.timerResult = 0;
        setTimeout(function(a, b, c) {
            globalThis.timerResult = a + b + c;
        }, 5, 10, 20, 30);
    )");

    TaggedValue pre = agent.GetRealm().GetGlobalProperty("timerResult");
    assert(pre.IsSmi() && pre.AsSmi() == 0);

    agent.RunEventLoop();

    TaggedValue post = agent.GetRealm().GetGlobalProperty("timerResult");
    assert(post.IsSmi() && post.AsSmi() == 60);

    // 2. Cancellation via clearTimeout
    agent.EvaluateScript(R"(
        globalThis.cancelledVal = 100;
        var id = setTimeout(function() {
            globalThis.cancelledVal = 999;
        }, 5);
        clearTimeout(id);
    )");

    agent.RunEventLoop();

    TaggedValue cancelled = agent.GetRealm().GetGlobalProperty("cancelledVal");
    assert(cancelled.IsSmi() && cancelled.AsSmi() == 100);

    // 3. Nested setTimeout chaining
    TaggedValue nestedRes = agent.EvaluateScriptWithEventLoop(R"(
        var step = 0;
        setTimeout(function() {
            step = 1;
            setTimeout(function() {
                step = 2;
            }, 5);
        }, 5);
        step;
    )");
    assert(nestedRes.IsSmi() && nestedRes.AsSmi() == 2);

    std::cout << "  -> Basic timers, argument forwarding, and clearTimeout verified.\n" << std::flush;
}

void TestIntervalTimersAndClearInterval() {
    std::cout << "[Test 2] Verifying setInterval and clearInterval Lifecycle...\n" << std::flush;

    Agent agent;

    // 1. Repeated setInterval terminated via clearInterval inside callback
    TaggedValue res = agent.EvaluateScriptWithEventLoop(R"(
        var count = 0;
        var intervalId = setInterval(function() {
            count = count + 1;
            if (count == 4) {
                clearInterval(intervalId);
            }
        }, 2);
        count;
    )");
    assert(res.IsSmi() && res.AsSmi() == 4);

    // 2. Multiple concurrent intervals
    agent.EvaluateScript(R"(
        globalThis.accA = 0;
        globalThis.accB = 0;
        var idA = setInterval(function() {
            globalThis.accA = globalThis.accA + 1;
            if (globalThis.accA == 3) clearInterval(idA);
        }, 2);
        var idB = setInterval(function() {
            globalThis.accB = globalThis.accB + 10;
            if (globalThis.accB == 20) clearInterval(idB);
        }, 3);
    )");

    agent.RunEventLoop();

    TaggedValue accA = agent.GetRealm().GetGlobalProperty("accA");
    TaggedValue accB = agent.GetRealm().GetGlobalProperty("accB");
    assert(accA.IsSmi() && accA.AsSmi() == 3);
    assert(accB.IsSmi() && accB.AsSmi() == 20);

    std::cout << "  -> setInterval and dynamic clearInterval verified.\n" << std::flush;
}

void TestImmediateTasks() {
    std::cout << "[Test 3] Verifying setImmediate and clearImmediate Tasks...\n" << std::flush;

    Agent agent;

    // 1. setImmediate with argument forwarding
    agent.EvaluateScript(R"(
        globalThis.immediateMsg = "";
        setImmediate(function(prefix, num) {
            globalThis.immediateMsg = prefix + num;
        }, "Item #", 42);
    )");

    agent.RunEventLoop();

    TaggedValue msg = agent.GetRealm().GetGlobalProperty("immediateMsg");
    assert(msg.IsHeapObject() && msg.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(msg.AsHeapObject())->Value() == "Item #42");

    // 2. clearImmediate cancels before execution
    agent.EvaluateScript(R"(
        globalThis.immFlag = 0;
        var id = setImmediate(function() {
            globalThis.immFlag = 1;
        });
        clearImmediate(id);
    )");

    agent.RunEventLoop();

    TaggedValue flag = agent.GetRealm().GetGlobalProperty("immFlag");
    assert(flag.IsSmi() && flag.AsSmi() == 0);

    std::cout << "  -> setImmediate and clearImmediate verified.\n" << std::flush;
}

void TestMicrotaskAndMacrotaskInterleaving() {
    std::cout << "[Test 4] Verifying Microtask / Macrotask Specification Interleaving...\n" << std::flush;

    Agent agent;

    // Verify ordering:
    // 1. Synchronous main script
    // 2. Microtask queued at root (queueMicrotask)
    // 3. First macrotask timer
    // 4. Microtask queued inside first macrotask timer
    // 5. Second macrotask timer
    agent.EvaluateScript(R"(
        globalThis.trace = [];

        setTimeout(function() {
            globalThis.trace.push("macro_timer1");
            queueMicrotask(function() {
                globalThis.trace.push("micro_in_timer1");
            });
        }, 0);

        setTimeout(function() {
            globalThis.trace.push("macro_timer2");
        }, 0);

        queueMicrotask(function() {
            globalThis.trace.push("micro_root");
        });

        globalThis.trace.push("sync_main");
    )");

    agent.RunEventLoop();

    TaggedValue traceVal = agent.GetRealm().GetGlobalProperty("trace");
    assert(traceVal.IsHeapObject() && traceVal.AsHeapObject()->IsArray());
    auto* traceArr = static_cast<JSArray*>(traceVal.AsHeapObject());
    assert(traceArr->Length() == 5);

    auto getElemStr = [&](uint32_t idx) -> std::string {
        TaggedValue el = traceArr->GetElement(idx);
        assert(el.IsHeapObject() && el.AsHeapObject()->IsString());
        return static_cast<JSString*>(el.AsHeapObject())->Value();
    };

    assert(getElemStr(0) == "sync_main");
    assert(getElemStr(1) == "micro_root");
    assert(getElemStr(2) == "macro_timer1");
    assert(getElemStr(3) == "micro_in_timer1");
    assert(getElemStr(4) == "macro_timer2");

    std::cout << "  -> Microtask / macrotask execution interleaving verified.\n" << std::flush;
}

void TestPromiseAsyncChainsInEventLoop() {
    std::cout << "[Test 5] Verifying Promises Resolved Inside Macrotask Timers...\n" << std::flush;

    Agent agent;
    TaggedValue res1 = agent.EvaluateScriptWithEventLoop(R"(
        var finalValue = 0;
        new Promise(function(resolve) {
            setTimeout(function() {
                resolve(21);
            }, 5);
        }).then(function(val) {
            return val * 2;
        }).then(function(doubled) {
            finalValue = doubled;
        });
        finalValue;
    )");
    assert(res1.IsSmi() && res1.AsSmi() == 42);

    // 2. Promise rejected asynchronously inside timer caught by catch
    TaggedValue res2 = agent.EvaluateScriptWithEventLoop(R"(
        var caughtMsg = "";
        new Promise(function(resolve, reject) {
            setTimeout(function() {
                reject("TimeoutRejected");
            }, 5);
        }).catch(function(err) {
            caughtMsg = err;
        });
        caughtMsg;
    )");
    assert(res2.IsHeapObject() && res2.AsHeapObject()->IsString());
    assert(static_cast<JSString*>(res2.AsHeapObject())->Value() == "TimeoutRejected");

    std::cout << "  -> Promise resolution and rejection in event loop verified.\n" << std::flush;
}

void TestPerformanceNowAndHighResolutionClock() {
    std::cout << "[Test 6] Verifying performance.now() Monotonic Time Progression...\n" << std::flush;

    Agent agent;

    TaggedValue res = agent.EvaluateScriptWithEventLoop(R"(
        var t1 = performance.now();
        var elapsed = 0;
        setTimeout(function() {
            var t2 = performance.now();
            elapsed = t2 - t1;
        }, 10);
        elapsed;
    )");

    assert(res.IsDouble() || res.IsSmi());
    double elapsedMs = res.AsNumber();
    assert(elapsedMs >= 5.0); // At least 5ms must have elapsed for a 10ms timer

    std::cout << "  -> performance.now() high-resolution monotonic timer verified.\n" << std::flush;
}

int main() {
    std::cout << "=========================================================\n";
    std::cout << " AetherVM Milestone 9 Test Suite: Asynchronous Event Loop\n";
    std::cout << "=========================================================\n\n";

    TestBasicTimersAndArguments();
    TestIntervalTimersAndClearInterval();
    TestImmediateTasks();
    TestMicrotaskAndMacrotaskInterleaving();
    TestPromiseAsyncChainsInEventLoop();
    TestPerformanceNowAndHighResolutionClock();

    std::cout << "\n[PASS] All 6 Milestone 9 Event Loop Verification Suites Passed!\n";
    return 0;
}
