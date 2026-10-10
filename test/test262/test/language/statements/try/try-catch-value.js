/*---
description: Try statement catch parameter binding and value propagation
esid: sec-try-statement
includes: [assert.js]
---*/

let caught = 0;
try {
    throw 42;
} catch (e) {
    caught = e;
}

assert.sameValue(caught, 42, "Caught thrown primitive value");
