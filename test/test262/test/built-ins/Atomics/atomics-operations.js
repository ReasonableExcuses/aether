/*---
description: Atomics load, store, add, sub, and, or, xor, exchange, compareExchange and isLockFree
features: [Atomics, SharedArrayBuffer]
includes: [assert.js]
---*/

var sab = new SharedArrayBuffer(16);
var i32 = new Int32Array(sab);

// 1. isLockFree
assert.sameValue(Atomics.isLockFree(4), true, "isLockFree(4) for 32-bit atomic integers");
assert.sameValue(Atomics.isLockFree(1), true, "isLockFree(1)");

// 2. store & load
assert.sameValue(Atomics.store(i32, 0, 100), 100, "store return value");
assert.sameValue(Atomics.load(i32, 0), 100, "load value");

// 3. add & sub
assert.sameValue(Atomics.add(i32, 0, 50), 100, "add returns old value");
assert.sameValue(Atomics.load(i32, 0), 150, "load after add");

assert.sameValue(Atomics.sub(i32, 0, 30), 150, "sub returns old value");
assert.sameValue(Atomics.load(i32, 0), 120, "load after sub");

// 4. Bitwise: and, or, xor
Atomics.store(i32, 1, 0b1100);
assert.sameValue(Atomics.and(i32, 1, 0b1010), 0b1100, "and returns old value");
assert.sameValue(Atomics.load(i32, 1), 0b1000, "load after and");

assert.sameValue(Atomics.or(i32, 1, 0b0011), 0b1000, "or returns old value");
assert.sameValue(Atomics.load(i32, 1), 0b1011, "load after or");

assert.sameValue(Atomics.xor(i32, 1, 0b1111), 0b1011, "xor returns old value");
assert.sameValue(Atomics.load(i32, 1), 0b0100, "load after xor");

// 5. exchange & compareExchange
assert.sameValue(Atomics.exchange(i32, 0, 999), 120, "exchange returns old value");
assert.sameValue(Atomics.load(i32, 0), 999, "load after exchange");

// compareExchange mismatch: expected 100 != 999 -> no change
assert.sameValue(Atomics.compareExchange(i32, 0, 100, 500), 999, "compareExchange mismatch old value");
assert.sameValue(Atomics.load(i32, 0), 999, "value unchanged on mismatch");

// compareExchange match: expected 999 == 999 -> updates to 500
assert.sameValue(Atomics.compareExchange(i32, 0, 999, 500), 999, "compareExchange match old value");
assert.sameValue(Atomics.load(i32, 0), 500, "value updated on match");

// 6. Non-shared buffer rejection
assert.throws(TypeError, function() {
    var regular = new Int32Array(new ArrayBuffer(16));
    Atomics.add(regular, 0, 1);
}, "Atomics on unshared ArrayBuffer throws TypeError");
