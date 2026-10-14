/*---
description: Number static methods (isInteger, isSafeInteger, isNaN, isFinite) and constants
features: [Number]
includes: [assert.js]
---*/

// 1. Constants
assert.sameValue(Number.MAX_SAFE_INTEGER, 9007199254740991, "MAX_SAFE_INTEGER");
assert.sameValue(Number.MIN_SAFE_INTEGER, -9007199254740991, "MIN_SAFE_INTEGER");
assert.sameValue(Number.EPSILON > 0 && Number.EPSILON < 0.0001, true, "EPSILON");

// 2. Number.isInteger
assert.sameValue(Number.isInteger(0), true, "isInteger(0)");
assert.sameValue(Number.isInteger(-100), true, "isInteger(-100)");
assert.sameValue(Number.isInteger(3.14), false, "isInteger(3.14)");
assert.sameValue(Number.isInteger(NaN), false, "isInteger(NaN)");
assert.sameValue(Number.isInteger(Infinity), false, "isInteger(Infinity)");
assert.sameValue(Number.isInteger("42"), false, "isInteger('42') non-coercing");

// 3. Number.isSafeInteger
assert.sameValue(Number.isSafeInteger(9007199254740991), true, "isSafeInteger(MAX_SAFE)");
assert.sameValue(Number.isSafeInteger(9007199254740992), false, "isSafeInteger(MAX_SAFE + 1)");

// 4. Number.isNaN
assert.sameValue(Number.isNaN(NaN), true, "Number.isNaN(NaN)");
assert.sameValue(Number.isNaN(123), false, "Number.isNaN(123)");
assert.sameValue(Number.isNaN("hello"), false, "Number.isNaN('hello') does not coerce to NaN");

// 5. Number.isFinite
assert.sameValue(Number.isFinite(100), true, "Number.isFinite(100)");
assert.sameValue(Number.isFinite(Infinity), false, "Number.isFinite(Infinity)");
assert.sameValue(Number.isFinite(-Infinity), false, "Number.isFinite(-Infinity)");
assert.sameValue(Number.isFinite("100"), false, "Number.isFinite('100') does not coerce");
