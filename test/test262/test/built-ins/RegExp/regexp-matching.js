/*---
description: RegExp literal, flags, test, exec and String integration
features: [RegExp]
includes: [assert.js]
---*/

// 1. RegExp literal and test
var re = /^[a-z]+[0-9]+$/i;
assert.sameValue(re.test("abc123"), true, "re.test matches valid alphanumeric pattern");
assert.sameValue(re.test("123abc"), false, "re.test rejects reversed pattern");
assert.sameValue(re.test("ABC456"), true, "case-insensitive flag matches uppercase");

// 2. RegExp.prototype.exec
var execRe = /(\w+)\s+(\d+)/;
var match = execRe.exec("Item 42");
assert.sameValue(match !== null, true, "exec matched");
assert.sameValue(match[0], "Item 42", "full match");
assert.sameValue(match[1], "Item", "capture group 1");
assert.sameValue(match[2], "42", "capture group 2");

// 3. String.prototype.replace with RegExp
var replaced = "apple and banana".replace(/a/g, "X");
assert.sameValue(replaced, "Xpple Xnd bXnXnX", "global replace");

// 4. String.prototype.split with RegExp
var parts = "one, two,   three".split(/,\s*/);
assert.sameValue(parts.length, 3, "split length");
assert.sameValue(parts[0], "one", "parts[0]");
assert.sameValue(parts[1], "two", "parts[1]");
assert.sameValue(parts[2], "three", "parts[2]");
