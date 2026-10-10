/*---
description: Map collection object operations set, get, has, delete, size
esid: sec-map-objects
features: [Map]
includes: [assert.js]
---*/

let m = new Map();
m.set("key1", 100);
m.set("key2", 200);

assert.sameValue(m.has("key1"), true, "Map has key1");
assert.sameValue(m.has("nonexistent"), false, "Map has nonexistent key");
assert.sameValue(m.get("key1"), 100, "Map get key1");
assert.sameValue(m.get("key2"), 200, "Map get key2");

m.delete("key1");
assert.sameValue(m.has("key1"), false, "Map key1 removed after delete");
