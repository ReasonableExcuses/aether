/*---
description: Comprehensive TypedArray methods and element operations
features: [TypedArray]
includes: [assert.js]
---*/

// 1. Instantiation of diverse TypedArray kinds
var u8 = new Uint8Array([10, 20, 30, 40, 50]);
var i16 = new Int16Array([-100, 200, -300, 400]);
var f32 = new Float32Array([1.5, 2.5, 3.5]);

assert.sameValue(u8.length, 5, "u8.length");
assert.sameValue(u8.byteLength, 5, "u8.byteLength");
assert.sameValue(i16.length, 4, "i16.length");
assert.sameValue(i16.byteLength, 8, "i16.byteLength");
assert.sameValue(f32.length, 3, "f32.length");
assert.sameValue(f32.byteLength, 12, "f32.byteLength");

// 2. Element access and mutation
assert.sameValue(u8[0], 10, "u8[0]");
assert.sameValue(u8[4], 50, "u8[4]");
assert.sameValue(u8[5], undefined, "u8[5] out of bounds");

u8[2] = 99;
assert.sameValue(u8[2], 99, "u8[2] updated");

// 3. Subarray (shared memory view)
var sub = u8.subarray(1, 4);
assert.sameValue(sub.length, 3, "sub.length");
assert.sameValue(sub[0], 20, "sub[0]");
assert.sameValue(sub[1], 99, "sub[1]");
assert.sameValue(sub[2], 40, "sub[2]");

sub[0] = 77;
assert.sameValue(u8[1], 77, "mutating subarray updates parent array");

// 4. Higher-order methods: map, filter, reduce
var mapped = u8.map(function(x) { return x * 2; });
assert.sameValue(mapped instanceof Uint8Array, true, "map returns TypedArray");
assert.sameValue(mapped[0], 20, "mapped[0]");
assert.sameValue(mapped[1], 154, "mapped[1]");

var sum = u8.reduce(function(acc, val) { return acc + val; }, 0);
assert.sameValue(sum, 10 + 77 + 99 + 40 + 50, "reduce sum");

// 5. Searching: indexOf, includes, find, findIndex
assert.sameValue(u8.indexOf(99), 2, "indexOf 99");
assert.sameValue(u8.indexOf(999), -1, "indexOf missing");
assert.sameValue(u8.includes(77), true, "includes 77");
assert.sameValue(u8.includes(888), false, "includes 888");

var found = u8.find(function(x) { return x > 50; });
assert.sameValue(found, 77, "find first element > 50");
var foundIdx = u8.findIndex(function(x) { return x > 50; });
assert.sameValue(foundIdx, 1, "findIndex first element > 50");

// 6. Bulk operations: fill, set, reverse, sort
var filled = new Uint8Array(4);
filled.fill(7);
assert.sameValue(filled[0], 7, "fill[0]");
assert.sameValue(filled[3], 7, "fill[3]");

var target = new Uint8Array(6);
target.set([1, 2, 3], 2);
assert.sameValue(target[0], 0, "set offset prefix");
assert.sameValue(target[2], 1, "set[2]");
assert.sameValue(target[3], 2, "set[3]");
assert.sameValue(target[4], 3, "set[4]");

var sortArr = new Uint8Array([50, 10, 40, 20, 30]);
sortArr.sort();
assert.sameValue(sortArr[0], 10, "sort[0]");
assert.sameValue(sortArr[1], 20, "sort[1]");
assert.sameValue(sortArr[4], 50, "sort[4]");

sortArr.reverse();
assert.sameValue(sortArr[0], 50, "reverse[0]");
assert.sameValue(sortArr[4], 10, "reverse[4]");
