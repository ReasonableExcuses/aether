// String append churn (ropes and string allocation stress)
let s = "";
for (let i = 0; i < 20000; i++) {
    s += "x";
}
let parts = [];
for (let i = 0; i < 10000; i++) {
    parts.push("token_" + (i % 50));
}
let joined = parts.join("-");
if (s.length !== 20000 || joined.length === 0) {
    throw new Error("STR assertion failed");
}
