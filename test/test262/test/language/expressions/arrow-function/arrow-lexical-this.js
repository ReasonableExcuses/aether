/*---
description: Arrow functions lexical this binding
esid: sec-arrow-function-definitions
features: [arrow-function]
includes: [assert.js]
---*/

let obj = {
    multiplier: 5,
    calc: function(val) {
        return val * this.multiplier;
    }
};

assert.sameValue(obj.calc(6), 30, "Method invocation with this property");
