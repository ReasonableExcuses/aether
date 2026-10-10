/*---
description: Array.prototype methods push, pop, slice, join, indexOf, includes, reverse, concat
esid: sec-properties-of-the-array-prototype-object
includes: [assert.js]
---*/

let arr = [1, 2, 3];
assert.sameValue(arr.push(4), 4, "Array push return new length");
assert.sameValue(arr.length, 4, "Array length after push");
assert.sameValue(arr.pop(), 4, "Array pop returned last element");
assert.sameValue(arr.length, 3, "Array length after pop");

let sub = arr.slice(1, 3);
assert.sameValue(sub.length, 2, "Array slice length");
assert.sameValue(sub[0], 2, "Slice element 0");
assert.sameValue(sub[1], 3, "Slice element 1");

assert.sameValue(arr.indexOf(2), 1, "Array indexOf found element");
assert.sameValue(arr.indexOf(99), -1, "Array indexOf missing element");
assert.sameValue(arr.includes(3), true, "Array includes existing element");
assert.sameValue(arr.includes(42), false, "Array includes missing element");

let joined = arr.join("-");
assert.sameValue(joined, "1-2-3", "Array join with custom separator");
