// DeltaBlue benchmark (clean compact constraint solver subset)
function Variable(name, initialValue) {
    this.name = name;
    this.value = initialValue;
    this.constraints = [];
    this.determinedBy = null;
    this.mark = 0;
}

function Constraint(v1, v2, strength) {
    this.v1 = v1;
    this.v2 = v2;
    this.strength = strength;
    v1.constraints.push(this);
    v2.constraints.push(this);
}

Constraint.prototype.satisfy = function(mark) {
    this.v2.value = this.v1.value + 1;
    this.v2.determinedBy = this;
    this.v2.mark = mark;
};

let vars = [];
for (let i = 0; i < 500; i++) {
    vars.push(new Variable("v" + i, i));
}

let constraints = [];
for (let i = 0; i < 499; i++) {
    constraints.push(new Constraint(vars[i], vars[i + 1], 1));
}

for (let round = 1; round <= 20; round++) {
    vars[0].value = round * 10;
    for (let i = 0; i < constraints.length; i++) {
        constraints[i].satisfy(round);
    }
}

if (vars[499].value !== 200 + 499) {
    throw new Error("DeltaBlue assertion failed: " + vars[499].value);
}
