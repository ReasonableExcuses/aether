/*---
description: Set collection object operations add, has, delete
esid: sec-set-objects
features: [Set]
includes: [assert.js]
---*/

let s = new Set();
s.add("alpha");
s.add("beta");

assert.sameValue(s.has("alpha"), true, "Set has alpha");
assert.sameValue(s.has("beta"), true, "Set has beta");
assert.sameValue(s.has("gamma"), false, "Set has gamma");

s.delete("alpha");
assert.sameValue(s.has("alpha"), false, "Set alpha deleted");
assert.sameValue(s.has("beta"), true, "Set beta still present");
