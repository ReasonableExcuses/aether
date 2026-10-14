/*---
description: Object static methods (assign, keys, values, entries, fromEntries, freeze, seal, is)
features: [Object]
includes: [assert.js]
---*/

// 1. Object.assign
var target = { a: 1 };
var result = Object.assign(target, { b: 2 }, { c: 3 });
assert.sameValue(result, target, "Object.assign returns target");
assert.sameValue(target.a, 1, "target.a");
assert.sameValue(target.b, 2, "target.b");
assert.sameValue(target.c, 3, "target.c");

// 2. Object.keys, values, entries
var sample = { x: 10, y: 20 };
var keys = Object.keys(sample);
assert.sameValue(keys.length, 2, "keys.length");
assert.sameValue(keys.indexOf("x") !== -1, true, "keys has x");
assert.sameValue(keys.indexOf("y") !== -1, true, "keys has y");

var values = Object.values(sample);
assert.sameValue(values.length, 2, "values.length");
assert.sameValue(values.indexOf(10) !== -1, true, "values has 10");
assert.sameValue(values.indexOf(20) !== -1, true, "values has 20");

var entries = Object.entries(sample);
assert.sameValue(entries.length, 2, "entries.length");

// 3. Object.fromEntries
var reconstructed = Object.fromEntries([["p", 100], ["q", 200]]);
assert.sameValue(reconstructed.p, 100, "reconstructed.p");
assert.sameValue(reconstructed.q, 200, "reconstructed.q");

// 4. Object.is
assert.sameValue(Object.is(42, 42), true, "Object.is(42, 42)");
assert.sameValue(Object.is(NaN, NaN), true, "Object.is(NaN, NaN)");
assert.sameValue(Object.is(0, -0), false, "Object.is(0, -0) is false");
assert.sameValue(Object.is(+0, -0), false, "Object.is(+0, -0) is false");

// 5. Object.freeze & Object.isFrozen
var frozen = Object.freeze({ fixed: 7 });
assert.sameValue(Object.isFrozen(frozen), true, "isFrozen(frozen)");
assert.sameValue(Object.isFrozen({}), false, "isFrozen({}) is false");

// 6. Object.seal & Object.isSealed
var sealed = Object.seal({ prop: 9 });
assert.sameValue(Object.isSealed(sealed), true, "isSealed(sealed)");

// 7. Object.hasOwn
assert.sameValue(Object.hasOwn({ own: 1 }, "own"), true, "hasOwn true");
assert.sameValue(Object.hasOwn({ own: 1 }, "toString"), false, "hasOwn false for inherited");
