/*---
description: Test262 official host object $262 specification compliance
includes: [assert.js]
---*/

assert.sameValue(typeof $262, "object", "$262 host object defined");
assert.sameValue(typeof $262.evalScript, "function", "$262.evalScript is a function");
assert.sameValue(typeof $262.gc, "function", "$262.gc is a function");
assert.sameValue(typeof $262.createRealm, "function", "$262.createRealm is a function");
assert.sameValue(typeof $262.agent, "object", "$262.agent host helper is defined");
assert.sameValue(typeof $262.agent.monotonic, "function", "$262.agent.monotonic is a function");

let evalResult = $262.evalScript("15 + 35;");
assert.sameValue(evalResult, 50, "$262.evalScript evaluates expression in realm");

$262.gc();
assert.sameValue(true, true, "$262.gc completed without exception");
