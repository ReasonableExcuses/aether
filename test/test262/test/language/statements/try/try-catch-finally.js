/*---
description: Try-catch-finally execution ordering
esid: sec-try-statement
includes: [assert.js]
---*/

let trace = 0;
try {
    trace = trace + 1;
    throw 10;
} catch (e) {
    trace = trace + e;
} finally {
    trace = trace + 100;
}

assert.sameValue(trace, 111, "Try, catch, and finally block sequential accumulation");
