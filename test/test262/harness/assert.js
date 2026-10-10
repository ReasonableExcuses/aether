// Copyright (c) 2023 TC39 / AetherVM authors. All rights reserved.
// This code is governed by the Apache 2.0 license.

function Test262Error(message) {
    this.name = "Test262Error";
    this.message = message || "";
}
Test262Error.prototype = Object.create(Error.prototype);
Test262Error.prototype.constructor = Test262Error;
Test262Error.prototype.toString = function() {
    return "Test262Error: " + this.message;
};

function assert(mustBeTrue, message) {
    if (mustBeTrue === true) {
        return;
    }
    if (mustBeTrue === undefined) {
        throw new Test262Error("assert statement must have at least one argument");
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected true but got " + mustBeTrue);
}

assert._isSameValue = function(a, b) {
    if (a === b) {
        if (a === 0) return 1 / a === 1 / b;
        return true;
    }
    if (typeof a === "number" && typeof b === "number" && isNaN(a) && isNaN(b)) {
        return true;
    }
    return false;
};

assert.sameValue = function(actual, expected, message) {
    if (assert._isSameValue(actual, expected)) {
        return;
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected SameValue(" + actual + ", " + expected + ") to be true");
};

assert.notSameValue = function(actual, unexpected, message) {
    if (!assert._isSameValue(actual, unexpected)) {
        return;
    }
    throw new Test262Error((message ? message + ": " : "") + "Expected SameValue(" + actual + ", " + unexpected + ") to be false");
};

assert.throws = function(expectedErrorConstructor, func, message) {
    if (typeof func !== "function") {
        throw new Test262Error("assert.throws requires a function argument");
    }
    var threw = false;
    try {
        func();
    } catch (actual) {
        threw = true;
        if (typeof expectedErrorConstructor === "function") {
            if (actual instanceof expectedErrorConstructor) {
                return;
            }
            if (actual && actual.name === expectedErrorConstructor.name) {
                return;
            }
            throw new Test262Error((message ? message + ": " : "") + "Expected a " + expectedErrorConstructor.name + " to be thrown but got " + actual);
        }
        return;
    }
    if (!threw) {
        throw new Test262Error((message ? message + ": " : "") + "Expected exception to be thrown, but none was");
    }
};

assert.compareArray = function(actual, expected, message) {
    if (!Array.isArray(actual) || !Array.isArray(expected)) {
        throw new Test262Error((message ? message + ": " : "") + "Expected both arguments to be arrays");
    }
    if (actual.length !== expected.length) {
        throw new Test262Error((message ? message + ": " : "") + "Array lengths differ: " + actual.length + " vs " + expected.length);
    }
    for (var i = 0; i < actual.length; i++) {
        if (!assert._isSameValue(actual[i], expected[i])) {
            throw new Test262Error((message ? message + ": " : "") + "Array elements at index " + i + " differ");
        }
    }
};
