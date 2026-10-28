// Recursion depth probe to verify stack limit guarding
let depth = 0;
function recurse() {
    depth++;
    recurse();
}

try {
    recurse();
} catch (e) {
    // Expect RangeError: Maximum call stack size exceeded
}

if (depth < 10) {
    throw new Error("Depth reached was too low: " + depth);
}
