// Copyright (c) 2023 TC39 / AetherVM authors. All rights reserved.
// This code is governed by the Apache 2.0 license.

var $ERROR = function(err) {
    if (typeof err === "string") {
        throw new Test262Error(err);
    }
    throw err;
};

var $DONE = function(err) {
    if (err) {
        $ERROR(err);
    }
};
