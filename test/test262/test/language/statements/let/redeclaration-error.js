/*---
description: SyntaxError thrown when encountering unexpected token in expression
esid: sec-let-and-const-declarations
negative:
  phase: parse
  type: SyntaxError
---*/

let x = 10 + * 20;
