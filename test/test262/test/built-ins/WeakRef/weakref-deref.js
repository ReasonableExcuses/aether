/*---
description: WeakRef constructor, deref and primitive target rejection
features: [WeakRef]
includes: [assert.js]
---*/

var target = { name: "AetherVM", release: 2026 };
var ref = new WeakRef(target);

// 1. deref returns target while strongly retained
assert.sameValue(ref.deref(), target, "ref.deref() returns target");

// 2. Symbol.toStringTag
assert.sameValue(Object.prototype.toString.call(ref), "[object WeakRef]", "Object.prototype.toString.call(ref)");

// 3. Reject non-objects
assert.throws(TypeError, function() {
    new WeakRef(12345);
}, "number as WeakRef target throws TypeError");

assert.throws(TypeError, function() {
    new WeakRef("string");
}, "string as WeakRef target throws TypeError");

assert.throws(TypeError, function() {
    new WeakRef(null);
}, "null as WeakRef target throws TypeError");
