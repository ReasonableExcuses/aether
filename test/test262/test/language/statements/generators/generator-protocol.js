/*---
description: Generator function execution, yield, next, return and done states
features: [generators]
includes: [assert.js]
---*/

// 1. Basic generator function
function* countToThree() {
    yield 1;
    yield 2;
    yield 3;
}

var gen = countToThree();
var r1 = gen.next();
assert.sameValue(r1.value, 1, "r1.value");
assert.sameValue(r1.done, false, "r1.done");

var r2 = gen.next();
assert.sameValue(r2.value, 2, "r2.value");
assert.sameValue(r2.done, false, "r2.done");

var r3 = gen.next();
assert.sameValue(r3.value, 3, "r3.value");
assert.sameValue(r3.done, false, "r3.done");

var r4 = gen.next();
assert.sameValue(r4.value, undefined, "r4.value after completion");
assert.sameValue(r4.done, true, "r4.done");

// 2. Generator iteration with for..of
var collected = [];
for (var val of countToThree()) {
    collected.push(val);
}
assert.sameValue(collected.length, 3, "collected.length");
assert.sameValue(collected[0], 1, "collected[0]");
assert.sameValue(collected[1], 2, "collected[1]");
assert.sameValue(collected[2], 3, "collected[2]");

// 3. Passing values into yield via next(val)
function* echo() {
    var a = yield 10;
    var b = yield a * 2;
    return b + 5;
}

var e = echo();
var step1 = e.next();
assert.sameValue(step1.value, 10, "step1.value");
var step2 = e.next(25); // a = 25 -> yields a * 2 = 50
assert.sameValue(step2.value, 50, "step2.value");
var step3 = e.next(7);  // b = 7 -> returns b + 5 = 12
assert.sameValue(step3.value, 12, "step3.value returned");
assert.sameValue(step3.done, true, "step3.done true on return");
