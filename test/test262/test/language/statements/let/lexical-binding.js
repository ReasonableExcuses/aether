/*---
description: Declarations in lexical declarations
esid: sec-let-and-const-declarations
features: [let]
includes: [assert.js]
---*/

let a = 10;
let b = 20;
let c = a + b;

assert.sameValue(a, 10, "Initial let value");
assert.sameValue(b, 20, "Initial let value");
assert.sameValue(c, 30, "Sum of let bindings");
