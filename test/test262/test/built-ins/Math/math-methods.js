/*---
description: Math object built-in intrinsic operations
esid: sec-math-object
includes: [assert.js]
---*/

assert.sameValue(Math.abs(-42), 42, "Math.abs negative integer");
assert.sameValue(Math.floor(4.9), 4, "Math.floor positive float");
assert.sameValue(Math.ceil(4.1), 5, "Math.ceil positive float");
assert.sameValue(Math.sqrt(144), 12, "Math.sqrt square root");
assert.sameValue(Math.max(10, 50, 30), 50, "Math.max multi-argument maximum");
assert.sameValue(Math.min(10, 50, 5), 5, "Math.min multi-argument minimum");
assert.sameValue(Math.pow(2, 10), 1024, "Math.pow exponentiation");
