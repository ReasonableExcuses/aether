/*---
description: Array.prototype higher order methods map, filter, forEach, reduce
esid: sec-properties-of-the-array-prototype-object
includes: [assert.js]
---*/

let nums = [1, 2, 3, 4, 5];

let doubled = nums.map(function(x) { return x * 2; });
assert.sameValue(doubled.length, 5, "Mapped array length");
assert.sameValue(doubled[0], 2, "Mapped element 0");
assert.sameValue(doubled[4], 10, "Mapped element 4");

let evens = nums.filter(function(x) { return x % 2 === 0; });
assert.sameValue(evens.length, 2, "Filtered array length");
assert.sameValue(evens[0], 2, "Filtered element 0");
assert.sameValue(evens[1], 4, "Filtered element 1");

let sum = nums.reduce(function(acc, x) { return acc + x; }, 0);
assert.sameValue(sum, 15, "Reduced array accumulation sum");
