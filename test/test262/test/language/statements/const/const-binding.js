/*---
description: Declarations in const declarations
esid: sec-let-and-const-declarations
features: [const]
includes: [assert.js]
---*/

const pi = 3.14159;
const radius = 10;
const area = pi * radius * radius;

assert.sameValue(pi, 3.14159, "const binding pi");
assert.sameValue(radius, 10, "const binding radius");
assert.sameValue(area, 314.159, "Computed area via const values");
