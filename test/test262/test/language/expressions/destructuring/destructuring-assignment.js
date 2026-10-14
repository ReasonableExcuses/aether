/*---
description: Array and object destructuring patterns with defaults and rest elements
features: [destructuring-binding, destructuring-assignment]
includes: [assert.js]
---*/

// 1. Array destructuring
var [a, b, c = 30] = [10, 20];
assert.sameValue(a, 10, "a");
assert.sameValue(b, 20, "b");
assert.sameValue(c, 30, "c default value");

// 2. Swapping via destructuring
var x = 1;
var y = 2;
[x, y] = [y, x];
assert.sameValue(x, 2, "swapped x");
assert.sameValue(y, 1, "swapped y");

// 3. Object destructuring
var { name, age, active = true } = { name: "Bob", age: 30 };
assert.sameValue(name, "Bob", "name");
assert.sameValue(age, 30, "age");
assert.sameValue(active, true, "active default value");

// 4. Nested destructuring
var { coords: { lat, lng } } = { coords: { lat: 37.77, lng: -122.41 } };
assert.sameValue(lat, 37.77, "lat");
assert.sameValue(lng, -122.41, "lng");
