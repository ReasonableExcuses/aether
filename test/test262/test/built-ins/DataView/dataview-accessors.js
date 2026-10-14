/*---
description: DataView multi-endian accessors, bounds checks, and float/int conversions
features: [DataView]
includes: [assert.js]
---*/

var buffer = new ArrayBuffer(16);
var dv = new DataView(buffer);

assert.sameValue(dv.byteLength, 16, "dv.byteLength");
assert.sameValue(dv.byteOffset, 0, "dv.byteOffset");

// 1. 8-bit integer reads and writes
dv.setInt8(0, -50);
assert.sameValue(dv.getInt8(0), -50, "getInt8");
assert.sameValue(dv.getUint8(0), 206, "getUint8 two's complement");

dv.setUint8(1, 255);
assert.sameValue(dv.getUint8(1), 255, "getUint8 255");
assert.sameValue(dv.getInt8(1), -1, "getInt8 -1");

// 2. 16-bit integer reads and writes (Big-Endian vs Little-Endian)
dv.setUint16(2, 0x1234, false); // Big-Endian
assert.sameValue(dv.getUint8(2), 0x12, "BE byte 0");
assert.sameValue(dv.getUint8(3), 0x34, "BE byte 1");
assert.sameValue(dv.getUint16(2, false), 0x1234, "getUint16 BE");
assert.sameValue(dv.getUint16(2, true), 0x3412, "getUint16 LE");

dv.setInt16(4, -1000, true); // Little-Endian
assert.sameValue(dv.getInt16(4, true), -1000, "getInt16 LE");

// 3. 32-bit integer reads and writes
dv.setInt32(6, 12345678, true);
assert.sameValue(dv.getInt32(6, true), 12345678, "getInt32 LE");

// 4. Floating point reads and writes
dv.setFloat32(10, 3.140000104904175, true);
assert.sameValue(Math.abs(dv.getFloat32(10, true) - 3.140000104904175) < 0.0001, true, "getFloat32");

// 5. Out of bounds throws RangeError
assert.throws(RangeError, function() {
    dv.getInt32(14); // 14 + 4 > 16
}, "getInt32 out of bounds");

assert.throws(RangeError, function() {
    dv.setInt8(16, 1); // offset >= length
}, "setInt8 out of bounds");
