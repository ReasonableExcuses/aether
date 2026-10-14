/*---
description: Optional chaining operator (?., ?.[] and ?.()) short-circuiting on nullish values
features: [optional-chaining]
includes: [assert.js]
---*/

var obj = {
    user: {
        address: {
            city: "San Francisco"
        },
        greet: function(msg) {
            return "Hello " + msg;
        }
    }
};

// 1. Existing paths
assert.sameValue(obj?.user?.address?.city, "San Francisco", "deep valid property");
assert.sameValue(obj?.user?.greet?.("World"), "Hello World", "valid method call");
assert.sameValue(obj?.user?.["address"]?.city, "San Francisco", "computed property access");

// 2. Short-circuiting on null/undefined
var empty = null;
assert.sameValue(empty?.user?.address?.city, undefined, "null target short-circuits");

var undef;
assert.sameValue(undef?.anything, undefined, "undefined target short-circuits");
assert.sameValue(obj?.missing?.deep?.prop, undefined, "missing intermediate property");
assert.sameValue(obj?.missingFunc?.(), undefined, "missing function call short-circuits without error");
