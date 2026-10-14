/*---
description: Symbol creation, Symbol.for registry, and well-known symbols
features: [Symbol]
includes: [assert.js]
---*/

// 1. Primitive Symbol creation
var s1 = Symbol("desc");
var s2 = Symbol("desc");
assert.sameValue(typeof s1, "symbol", "typeof s1");
assert.sameValue(s1 === s2, false, "distinct symbols with same description are not equal");

// 2. Symbol.for global registry
var g1 = Symbol.for("app.key");
var g2 = Symbol.for("app.key");
assert.sameValue(g1 === g2, true, "Symbol.for returns same registered symbol");
assert.sameValue(Symbol.keyFor(g1), "app.key", "Symbol.keyFor");
assert.sameValue(Symbol.keyFor(s1), undefined, "Symbol.keyFor on non-global symbol returns undefined");

// 3. Well-known symbols
assert.sameValue(typeof Symbol.iterator, "symbol", "Symbol.iterator is symbol");
assert.sameValue(typeof Symbol.asyncIterator, "symbol", "Symbol.asyncIterator is symbol");
assert.sameValue(typeof Symbol.toStringTag, "symbol", "Symbol.toStringTag is symbol");
assert.sameValue(typeof Symbol.hasInstance, "symbol", "Symbol.hasInstance is symbol");

// 4. Custom iterable using Symbol.iterator
var customIterable = {};
customIterable[Symbol.iterator] = function() {
    var count = 0;
    return {
        next: function() {
            if (count < 3) {
                return { value: ++count, done: false };
            }
            return { value: undefined, done: true };
        }
    };
};

var gathered = [];
for (var item of customIterable) {
    gathered.push(item);
}
assert.sameValue(gathered.length, 3, "gathered.length");
assert.sameValue(gathered[0], 1, "gathered[0]");
assert.sameValue(gathered[1], 2, "gathered[1]");
assert.sameValue(gathered[2], 3, "gathered[2]");
