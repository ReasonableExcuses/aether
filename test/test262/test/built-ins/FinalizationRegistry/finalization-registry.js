/*---
description: FinalizationRegistry registration, unregistration and constructor validation
features: [FinalizationRegistry]
includes: [assert.js]
---*/

var cleanupCalled = false;
var registry = new FinalizationRegistry(function(heldValue) {
    cleanupCalled = true;
});

var t1 = { id: 1 };
var t2 = { id: 2 };
var tok1 = { token: "tok1" };
var tok2 = { token: "tok2" };

// 1. register
registry.register(t1, "resource1", tok1);
registry.register(t2, "resource2", tok2);

// 2. unregister
assert.sameValue(registry.unregister(tok1), true, "unregister with existing token returns true");
assert.sameValue(registry.unregister(tok1), false, "unregister second time returns false");

// 3. Constructor requires function
assert.throws(TypeError, function() {
    new FinalizationRegistry("not-a-function");
}, "non-function cleanup callback throws TypeError");

// 4. Primitive target rejection
assert.throws(TypeError, function() {
    registry.register(12345, "held");
}, "primitive target throws TypeError");

// 5. Primitive unregisterToken rejection
assert.throws(TypeError, function() {
    registry.unregister("primitive-token");
}, "primitive unregisterToken throws TypeError");
