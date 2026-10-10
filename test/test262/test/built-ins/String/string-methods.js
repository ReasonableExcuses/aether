/*---
description: String.prototype methods charAt, indexOf, slice, substring, toLowerCase, toUpperCase, trim, split, includes
esid: sec-properties-of-the-string-prototype-object
includes: [assert.js]
---*/

let s = "  AetherVM Engine  ";

let trimmed = s.trim();
assert.sameValue(trimmed, "AetherVM Engine", "String trim removes surrounding whitespace");

assert.sameValue(trimmed.charAt(0), "A", "charAt index 0");
assert.sameValue(trimmed.indexOf("VM"), 6, "indexOf substring VM");
assert.sameValue(trimmed.includes("Engine"), true, "includes substring Engine");
assert.sameValue(trimmed.includes("V8"), false, "includes missing substring");

let upper = trimmed.toUpperCase();
assert.sameValue(upper, "AETHERVM ENGINE", "toUpperCase conversion");

let lower = trimmed.toLowerCase();
assert.sameValue(lower, "aethervm engine", "toLowerCase conversion");

let parts = trimmed.split(" ");
assert.sameValue(parts.length, 2, "String split into parts");
assert.sameValue(parts[0], "AetherVM", "Split part 0");
assert.sameValue(parts[1], "Engine", "Split part 1");
