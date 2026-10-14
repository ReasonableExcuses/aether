/*---
description: ECMA-402 Intl.NumberFormat, DateTimeFormat, Collator, and PluralRules
features: [Intl]
includes: [assert.js]
---*/

// 1. Intl.NumberFormat
var nf = new Intl.NumberFormat("en-US");
var formattedNum = nf.format(1234567.89);
assert.sameValue(typeof formattedNum, "string", "NumberFormat format returns string");
assert.sameValue(formattedNum.indexOf("1,234,567") !== -1, true, "NumberFormat contains grouping");

// 2. Intl.DateTimeFormat
var dtf = new Intl.DateTimeFormat("en-US");
var formattedDate = dtf.format(new Date(1600000000000));
assert.sameValue(typeof formattedDate, "string", "DateTimeFormat returns string");

// 3. Intl.Collator
var collator = new Intl.Collator("en-US");
assert.sameValue(collator.compare("a", "b") < 0, true, "a < b");
assert.sameValue(collator.compare("b", "a") > 0, true, "b > a");
assert.sameValue(collator.compare("abc", "abc"), 0, "abc === abc");

// 4. Intl.PluralRules
var pr = new Intl.PluralRules("en-US");
assert.sameValue(pr.select(1), "one", "PluralRules select 1 is 'one'");
assert.sameValue(pr.select(2), "other", "PluralRules select 2 is 'other'");
assert.sameValue(pr.select(0), "other", "PluralRules select 0 is 'other'");
