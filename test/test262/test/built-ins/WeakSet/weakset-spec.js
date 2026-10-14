/*---
description: WeakSet add, has, delete and primitive value rejection
features: [WeakSet]
includes: [assert.js]
---*/

var ws = new WeakSet();
var o1 = { a: 1 };
var o2 = { b: 2 };
var o3 = { c: 3 };

// 1. Initial membership
assert.sameValue(ws.has(o1), false, "initial has(o1)");

// 2. add & has
assert.sameValue(ws.add(o1), ws, "add returns WeakSet");
ws.add(o2);

assert.sameValue(ws.has(o1), true, "has(o1) after add");
assert.sameValue(ws.has(o2), true, "has(o2) after add");
assert.sameValue(ws.has(o3), false, "has(o3) not present");

// 3. delete
assert.sameValue(ws.delete(o1), true, "delete(o1) present object");
assert.sameValue(ws.has(o1), false, "has(o1) after delete");
assert.sameValue(ws.delete(o1), false, "delete(o1) absent object");

// 4. Primitive value rejection throws TypeError
assert.throws(TypeError, function() {
    ws.add(100);
}, "number as value throws TypeError");

assert.throws(TypeError, function() {
    ws.add("primitive");
}, "string as value throws TypeError");

assert.throws(TypeError, function() {
    ws.add(true);
}, "boolean as value throws TypeError");
