/*---
description: Iteration statements for loops
esid: sec-for-statement
includes: [assert.js]
---*/

let sum = 0;
for (let i = 1; i <= 10; i++) {
    sum = sum + i;
}

assert.sameValue(sum, 55, "Sum from 1 to 10 in for loop");
