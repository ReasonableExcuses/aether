// Live set retention: 100k long-lived retained array + 900k short-lived churn
let retained = [];
for (let i = 0; i < 100000; i++) {
    retained.push({ id: i, value: i * 7 });
}
let temp = null;
for (let i = 0; i < 900000; i++) {
    temp = { churn: i, text: "churn" };
}
if (retained.length !== 100000 || !temp) {
    throw new Error("GC2 assertion failed");
}
