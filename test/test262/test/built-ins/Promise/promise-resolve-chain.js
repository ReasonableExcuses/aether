/*---
description: Promise resolution, chaining, and microtask queue draining
esid: sec-promise-objects
features: [Promise]
flags: [async]
includes: [assert.js, sta.js]
---*/

Promise.resolve(10)
    .then(function(v) {
        return v * 2;
    })
    .then(function(v) {
        assert.sameValue(v + 5, 25, "Chained promise microtask chain resolution");
        $DONE();
    });
