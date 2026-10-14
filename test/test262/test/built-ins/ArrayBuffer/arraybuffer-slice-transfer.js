/*---
description: ArrayBuffer allocation, slice, isView and byteLength tests
features: [ArrayBuffer]
includes: [assert.js]
---*/

// 1. ArrayBuffer constructor & byteLength
var ab = new ArrayBuffer(16);
assert.sameValue(ab.byteLength, 16, "ArrayBuffer.byteLength");

// 2. ArrayBuffer.isView
var u8 = new Uint8Array(ab);
assert.sameValue(ArrayBuffer.isView(u8), true, "ArrayBuffer.isView(TypedArray)");
assert.sameValue(ArrayBuffer.isView(ab), false, "ArrayBuffer.isView(ArrayBuffer)");
assert.sameValue(ArrayBuffer.isView({}), false, "ArrayBuffer.isView(plain object)");
assert.sameValue(ArrayBuffer.isView(null), false, "ArrayBuffer.isView(null)");

// 3. ArrayBuffer.prototype.slice
u8[0] = 11;
u8[1] = 22;
u8[2] = 33;
u8[3] = 44;

var slicedAb = ab.slice(1, 3);
assert.sameValue(slicedAb instanceof ArrayBuffer, true, "slice returns ArrayBuffer");
assert.sameValue(slicedAb.byteLength, 2, "slicedAb.byteLength");

var slicedView = new Uint8Array(slicedAb);
assert.sameValue(slicedView[0], 22, "slicedView[0]");
assert.sameValue(slicedView[1], 33, "slicedView[1]");

// Ensure slice creates an independent copy
slicedView[0] = 99;
assert.sameValue(u8[1], 22, "original buffer unaltered by sliced buffer mutation");
