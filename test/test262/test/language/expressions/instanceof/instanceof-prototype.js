/*---
description: Instanceof operator checks against prototype inheritance chain
esid: sec-instanceof-operator
includes: [assert.js]
---*/

let err = new TypeError("invalid type specified");
assert.sameValue(err instanceof TypeError, true, "err instanceof TypeError");
assert.sameValue(err.name, "TypeError", "TypeError name property");
assert.sameValue(err.message, "invalid type specified", "TypeError message property");

function Base() {}
let b = new Base();
assert.sameValue(b instanceof Base, true, "b instanceof Base");
assert.sameValue(b instanceof TypeError, false, "b is not an instance of TypeError");
