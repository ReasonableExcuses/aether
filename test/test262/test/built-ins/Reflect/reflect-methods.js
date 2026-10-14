/*---
description: Reflect get, set, has, deleteProperty, apply, construct, ownKeys
features: [Reflect]
includes: [assert.js]
---*/

var obj = { a: 1, b: 2 };

// 1. Reflect.get & Reflect.set
assert.sameValue(Reflect.get(obj, "a"), 1, "Reflect.get");
assert.sameValue(Reflect.set(obj, "c", 3), true, "Reflect.set");
assert.sameValue(obj.c, 3, "obj.c updated");

// 2. Reflect.has
assert.sameValue(Reflect.has(obj, "b"), true, "Reflect.has true");
assert.sameValue(Reflect.has(obj, "nonexistent"), false, "Reflect.has false");

// 3. Reflect.deleteProperty
assert.sameValue(Reflect.deleteProperty(obj, "b"), true, "Reflect.deleteProperty");
assert.sameValue("b" in obj, false, "b deleted");

// 4. Reflect.apply
function multiply(x, y) { return x * y; }
assert.sameValue(Reflect.apply(multiply, undefined, [6, 7]), 42, "Reflect.apply");

// 5. Reflect.construct
function Person(name) {
    this.name = name;
}
var p = Reflect.construct(Person, ["Alice"]);
assert.sameValue(p instanceof Person, true, "p instanceof Person");
assert.sameValue(p.name, "Alice", "p.name");

// 6. Reflect.ownKeys
var keys = Reflect.ownKeys({ x: 10, y: 20 });
assert.sameValue(keys.indexOf("x") !== -1, true, "keys includes x");
assert.sameValue(keys.indexOf("y") !== -1, true, "keys includes y");
