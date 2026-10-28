// Allocation churn: allocate 1,000,000 small objects
let last = null;
for (let i = 0; i < 1000000; i++) {
    last = { id: i, x: i * 2, y: i * 3 };
}
if (!last || last.id !== 999999) {
    throw new Error("GC1 assertion failed");
}
