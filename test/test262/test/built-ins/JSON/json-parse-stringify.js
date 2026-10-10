/*---
description: JSON object parse and stringify
esid: sec-json-object
includes: [assert.js]
---*/

let jsonStr = '{"name":"Aether","version":1}';
let obj = JSON.parse(jsonStr);

assert.sameValue(obj.name, "Aether", "Parsed JSON object property name");
assert.sameValue(obj.version, 1, "Parsed JSON object property version");

let serialized = JSON.stringify(obj);
let reparsed = JSON.parse(serialized);
assert.sameValue(reparsed.name, "Aether", "Roundtrip JSON serialize and parse");
assert.sameValue(reparsed.version, 1, "Roundtrip JSON serialize and parse version");
