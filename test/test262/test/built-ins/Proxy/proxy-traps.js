/*---
description: Proxy get, set, has, deleteProperty, apply, and construct traps
features: [Proxy, Reflect]
includes: [assert.js]
---*/

// 1. get & set traps
var targetObj = { x: 10, y: 20 };
var getLogs = [];
var setLogs = [];

var pObj = new Proxy(targetObj, {
    get: function(target, prop, receiver) {
        getLogs.push(prop);
        return prop in target ? target[prop] * 2 : 0;
    },
    set: function(target, prop, value, receiver) {
        setLogs.push(prop + "=" + value);
        target[prop] = value;
        return true;
    }
});

assert.sameValue(pObj.x, 20, "proxy get x");
assert.sameValue(pObj.y, 40, "proxy get y");
assert.sameValue(pObj.z, 0, "proxy get non-existent z");
assert.sameValue(getLogs.length, 3, "get trap triggered 3 times");

pObj.x = 50;
assert.sameValue(targetObj.x, 50, "proxy set x");
assert.sameValue(setLogs[0], "x=50", "set trap logged");

// 2. has trap (in operator)
var pHas = new Proxy({ a: 1 }, {
    has: function(target, prop) {
        return prop === "allowed";
    }
});
assert.sameValue("a" in pHas, false, "has trap overridden for existing key");
assert.sameValue("allowed" in pHas, true, "has trap returns true for allowed");

// 3. deleteProperty trap
var pDel = new Proxy({ secret: 42 }, {
    deleteProperty: function(target, prop) {
        if (prop === "secret") return false;
        delete target[prop];
        return true;
    }
});
assert.sameValue(delete pDel.secret, false, "deleteProperty trap prevents deletion");

// 4. apply trap (callable proxy)
function add(a, b) { return a + b; }
var pFunc = new Proxy(add, {
    apply: function(target, thisArg, args) {
        return target(args[0], args[1]) * 10;
    }
});
assert.sameValue(pFunc(2, 3), 50, "proxy apply trap");

// 5. construct trap
function Point(x, y) {
    this.x = x;
    this.y = y;
}
var pCtor = new Proxy(Point, {
    construct: function(target, args, newTarget) {
        var obj = new target(args[0], args[1]);
        obj.z = 99;
        return obj;
    }
});
var pt = new pCtor(1, 2);
assert.sameValue(pt.x, 1, "pt.x");
assert.sameValue(pt.y, 2, "pt.y");
assert.sameValue(pt.z, 99, "pt.z from construct trap");
