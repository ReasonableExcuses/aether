#undef NDEBUG
#include <cassert>

#include "aether/runtime.h"
#include "aether/heap.h"

#include <iostream>
#include <string>

using namespace aether;

void TestSymbolAsyncIterator() {
    std::cout << "[Test 1] Verifying Symbol.asyncIterator registration and properties...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let symAsync = Symbol.asyncIterator;
        let symSync = Symbol.iterator;

        let t1 = (typeof symAsync === "symbol");
        let t2 = (symAsync !== symSync);
        let t3 = (symAsync === Symbol.asyncIterator);

        let customObj = {};
        customObj[symAsync] = 42;
        let t4 = (customObj[Symbol.asyncIterator] === 42);

        let ok = (t1 && t2 && t3 && t4);
        ok;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Symbol.asyncIterator primitive and indexing verified.\n" << std::flush;
}

void TestAsyncGeneratorBasicYieldAndNext() {
    std::cout << "[Test 2] Verifying async function* yield and next() Promise resolution...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* gen() {
            yield 10;
            yield 20;
            return 30;
        }

        let g = gen();
        let pass = false;

        async function run() {
            let res1 = await g.next();
            let res2 = await g.next();
            let res3 = await g.next();
            if (res1.value === 10 && !res1.done &&
                res2.value === 20 && !res2.done &&
                res3.value === 30 && res3.done) {
                pass = true;
            }
        }

        run();
        pass;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Async generator yield and completion verified.\n" << std::flush;
}

void TestAsyncGeneratorTwoWayCommunication() {
    std::cout << "[Test 3] Verifying two-way communication via g.next(val)...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* calc() {
            let a = yield 1;
            let b = yield (a + 10);
            return b * 2;
        }

        let g = calc();
        let final_val = 0;

        async function run() {
            let s1 = await g.next();       // yields 1
            let s2 = await g.next(5);      // sends 5 into a, yields 15
            let s3 = await g.next(7);      // sends 7 into b, returns 14
            final_val = s1.value + s2.value + s3.value; // 1 + 15 + 14 = 30
        }

        run();
        final_val;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 30);
    std::cout << "  -> Two-way communication into yield expressions verified.\n" << std::flush;
}

void TestAsyncGeneratorInternalAwait() {
    std::cout << "[Test 4] Verifying internal await inside async function* without premature yields...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* asyncCompute() {
            let x = await Promise.resolve(100);
            let y = await Promise.resolve(50);
            yield (x + y);
            let z = await Promise.resolve(20);
            return (x + y + z);
        }

        let g = asyncCompute();
        let total = 0;

        async function run() {
            let r1 = await g.next();
            let r2 = await g.next();
            total = r1.value + r2.value; // 150 + 170 = 320
        }

        run();
        total;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 320);
    std::cout << "  -> Internal await pauses within async generator verified.\n" << std::flush;
}

void TestAsyncGeneratorSequentialQueueDraining() {
    std::cout << "[Test 5] Verifying sequential queue draining for concurrent next() calls...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* queueGen() {
            await Promise.resolve(1);
            yield 111;
            await Promise.resolve(2);
            yield 222;
            return 333;
        }

        let g = queueGen();
        let p1 = g.next();
        let p2 = g.next();
        let p3 = g.next();

        let total = 0;

        async function run() {
            let res1 = await p1;
            let res2 = await p2;
            let res3 = await p3;
            total = res1.value + res2.value + res3.value; // 111 + 222 + 333 = 666
        }

        run();
        total;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 666);
    std::cout << "  -> Sequential request queuing and FIFO resolution verified.\n" << std::flush;
}

void TestAsyncGeneratorReturnEarlyTermination() {
    std::cout << "[Test 6] Verifying return(val) early termination on async generator...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* numbers() {
            yield 1;
            yield 2;
            yield 3;
        }

        let g = numbers();
        let pass = false;

        async function run() {
            let s1 = await g.next();
            let s2 = await g.return(999);
            let s3 = await g.next();
            if (s1.value === 1 && !s1.done &&
                s2.value === 999 && s2.done &&
                s3.value === undefined && s3.done) {
                pass = true;
            }
        }

        run();
        pass;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsBoolean() && result.AsBoolean());
    std::cout << "  -> Early return settlement and generator termination verified.\n" << std::flush;
}

void TestAsyncGeneratorThrowExceptionHandling() {
    std::cout << "[Test 7] Verifying throw(err) and try-catch handling in async generator...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* errorGen() {
            let recovered = 0;
            try {
                yield 10;
            } catch (e) {
                recovered = e;
            }
            yield recovered;
            return 99;
        }

        let g = errorGen();
        let total = 0;

        async function run() {
            let s1 = await g.next();
            let s2 = await g.throw(555);
            let s3 = await g.next();
            total = s1.value + s2.value + s3.value; // 10 + 555 + 99 = 664
        }

        run();
        total;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 664);
    std::cout << "  -> Throw propagation and catch recovery verified.\n" << std::flush;
}

void TestForAwaitOfAsyncGenerator() {
    std::cout << "[Test 8] Verifying for await...of loop over async function*...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        async function* stream() {
            yield 5;
            yield 15;
            yield 25;
        }

        let sum = 0;

        async function run() {
            for await (const item of stream()) {
                sum += item;
            }
        }

        run();
        sum;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 45);
    std::cout << "  -> for await...of iteration over async generator verified.\n" << std::flush;
}

void TestForAwaitOfCustomAsyncIterable() {
    std::cout << "[Test 9] Verifying for await...of loop over custom object with [Symbol.asyncIterator]...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let customAsyncIterable = {
            [Symbol.asyncIterator]() {
                let current = 1;
                return {
                    next() {
                        if (current <= 3) {
                            let val = current * 10;
                            current++;
                            return Promise.resolve({ value: val, done: false });
                        }
                        return Promise.resolve({ value: undefined, done: true });
                    }
                };
            }
        };

        let total = 0;

        async function run() {
            for await (const x of customAsyncIterable) {
                total += x;
            }
        }

        run();
        total;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 60); // 10 + 20 + 30
    std::cout << "  -> for await...of custom [Symbol.asyncIterator] iteration verified.\n" << std::flush;
}

void TestForAwaitOfSyncIterableFallback() {
    std::cout << "[Test 10] Verifying for await...of fallback over synchronous array iterable...\n" << std::flush;

    Agent agent;

    std::string script = R"(
        let syncArray = [2, 4, 6, 8];
        let sum = 0;

        async function run() {
            for await (const num of syncArray) {
                sum += num;
            }
        }

        run();
        sum;
    )";

    TaggedValue result = agent.EvaluateScript(script);
    assert(result.IsSmi() && result.AsSmi() == 20);
    std::cout << "  -> for await...of sync iterable fallback verified.\n" << std::flush;
}

int main() {
    std::cout << "=================================================================\n";
    std::cout << "Running Aether Milestone 16 Test Suite: Async Iterators & Generators\n";
    std::cout << "=================================================================\n" << std::flush;

    TestSymbolAsyncIterator();
    TestAsyncGeneratorBasicYieldAndNext();
    TestAsyncGeneratorTwoWayCommunication();
    TestAsyncGeneratorInternalAwait();
    TestAsyncGeneratorSequentialQueueDraining();
    TestAsyncGeneratorReturnEarlyTermination();
    TestAsyncGeneratorThrowExceptionHandling();
    TestForAwaitOfAsyncGenerator();
    TestForAwaitOfCustomAsyncIterable();
    TestForAwaitOfSyncIterableFallback();

    std::cout << "=================================================================\n";
    std::cout << "All Milestone 16 Async Iterator & Generator Tests Passed (10/10)!\n";
    std::cout << "=================================================================\n" << std::flush;
    return 0;
}
