/*---
description: Reassigning to a const binding throws TypeError
features: [const]
negative:
  phase: runtime
  type: TypeError
---*/

const x = 42;
x = 99;
