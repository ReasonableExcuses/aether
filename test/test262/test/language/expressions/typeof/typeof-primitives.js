/*---
description: Typeof operator results on ECMAScript primitives and objects
esid: sec-typeof-operator
includes: [assert.js]
---*/

assert.sameValue(typeof 123, "number", "typeof number");
assert.sameValue(typeof "text", "string", "typeof string");
assert.sameValue(typeof true, "boolean", "typeof boolean");
assert.sameValue(typeof undefined, "undefined", "typeof undefined");
assert.sameValue(typeof null, "object", "typeof null returns object per specification");
assert.sameValue(typeof function(){}, "function", "typeof function");
assert.sameValue(typeof {}, "object", "typeof object");
