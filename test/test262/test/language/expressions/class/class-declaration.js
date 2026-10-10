/*---
description: Class definitions and constructor instantiation
esid: sec-class-definitions
features: [class]
includes: [assert.js]
---*/

class Rectangle {
    constructor(w, h) {
        this.w = w;
        this.h = h;
    }
    area() {
        return this.w * this.h;
    }
}

let rect = new Rectangle(7, 8);
assert.sameValue(rect.w, 7, "Rectangle width");
assert.sameValue(rect.h, 8, "Rectangle height");
assert.sameValue(rect.area(), 56, "Rectangle area calculation");
