/*---
description: Template literals with string interpolation and expressions
features: [template-literals]
includes: [assert.js]
---*/

// 1. Basic template literal
var simple = `Hello world`;
assert.sameValue(simple, "Hello world", "simple template literal");

// 2. Interpolation
var user = "Alice";
var greeting = `Hello, ${user}!`;
assert.sameValue(greeting, "Hello, Alice!", "variable interpolation");

// 3. Expression interpolation
var a = 10;
var b = 20;
var expr = `${a} + ${b} = ${a + b}`;
assert.sameValue(expr, "10 + 20 = 30", "arithmetic expression in template");

// 4. Multiline strings
var multiline = `Line 1
Line 2`;
assert.sameValue(multiline, "Line 1\nLine 2", "multiline template string preserves newline");
