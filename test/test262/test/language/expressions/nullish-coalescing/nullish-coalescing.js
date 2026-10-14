/*---
description: Nullish coalescing operator (??) evaluating left-hand operand unless null or undefined
features: [nullish-coalescing-operator]
includes: [assert.js]
---*/

// 1. Returns right-hand side when left-hand side is null or undefined
assert.sameValue(null ?? "default", "default", "null ?? default");
assert.sameValue(undefined ?? "default", "default", "undefined ?? default");

// 2. Returns left-hand side for falsy values that are NOT nullish
assert.sameValue(0 ?? 100, 0, "0 is preserved");
assert.sameValue(false ?? true, false, "false is preserved");
assert.sameValue("" ?? "default", "", "empty string is preserved");
assert.sameValue(NaN ?? 42, NaN, "NaN is preserved");

// 3. Short-circuiting: right-hand expression not evaluated if left is non-nullish
var sideEffect = 0;
var result = 42 ?? (++sideEffect);
assert.sameValue(result, 42, "result is 42");
assert.sameValue(sideEffect, 0, "right-hand side effect not evaluated");
