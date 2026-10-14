/*---
description: SharedArrayBuffer allocation, growable property, grow and slicing
features: [SharedArrayBuffer]
includes: [assert.js]
---*/

// 1. Basic non-growable SharedArrayBuffer
var sab = new SharedArrayBuffer(32);
assert.sameValue(sab.byteLength, 32, "sab.byteLength");

// 2. SharedArrayBuffer slice
var u8 = new Uint8Array(sab);
u8[0] = 42;
u8[1] = 43;

var subSab = sab.slice(0, 2);
assert.sameValue(subSab instanceof SharedArrayBuffer, true, "slice returns SharedArrayBuffer");
assert.sameValue(subSab.byteLength, 2, "subSab.byteLength");

var subU8 = new Uint8Array(subSab);
assert.sameValue(subU8[0], 42, "subU8[0]");
assert.sameValue(subU8[1], 43, "subU8[1]");

// 3. Growable SharedArrayBuffer
var growableSab = new SharedArrayBuffer(16, { maxByteLength: 64 });
assert.sameValue(growableSab.byteLength, 16, "growableSab initial length");
assert.sameValue(growableSab.maxByteLength, 64, "growableSab.maxByteLength");
assert.sameValue(growableSab.growable, true, "growableSab.growable");

growableSab.grow(32);
assert.sameValue(growableSab.byteLength, 32, "growableSab grown length");

// Exceeding maxByteLength throws RangeError
assert.throws(RangeError, function() {
    growableSab.grow(128);
}, "grow exceeding maxByteLength");
