/*---
description: WeakMap set, get, has, delete and primitive key rejection
features: [WeakMap]
includes: [assert.js]
---*/

var wm = new WeakMap();
var k1 = { id: 1 };
var k2 = { id: 2 };
var k3 = { id: 3 };

// 1. Initial empty checks
assert.sameValue(wm.has(k1), false, "initial has(k1)");
assert.sameValue(wm.get(k1), undefined, "initial get(k1)");

// 2. set & get
assert.sameValue(wm.set(k1, "hello"), wm, "set returns WeakMap");
wm.set(k2, 12345);

assert.sameValue(wm.has(k1), true, "has(k1) after set");
assert.sameValue(wm.get(k1), "hello", "get(k1) after set");
assert.sameValue(wm.has(k2), true, "has(k2)");
assert.sameValue(wm.get(k2), 12345, "get(k2)");
assert.sameValue(wm.has(k3), false, "has(k3) not present");

// 3. Overwriting key
wm.set(k1, "updated");
assert.sameValue(wm.get(k1), "updated", "get(k1) updated value");

// 4. delete
assert.sameValue(wm.delete(k1), true, "delete(k1) existing key");
assert.sameValue(wm.has(k1), false, "has(k1) after delete");
assert.sameValue(wm.get(k1), undefined, "get(k1) after delete");
assert.sameValue(wm.delete(k1), false, "delete(k1) already removed");

// 5. Primitive key rejection throws TypeError
assert.throws(TypeError, function() {
    wm.set(42, "fail");
}, "number as key throws TypeError");

assert.throws(TypeError, function() {
    wm.set("stringKey", "fail");
}, "string as key throws TypeError");

assert.throws(TypeError, function() {
    wm.set(null, "fail");
}, "null as key throws TypeError");
