// N-body gravitational simulation benchmark
function Body(x, y, z, vx, vy, vz, mass) {
    this.x = x;
    this.y = y;
    this.z = z;
    this.vx = vx;
    this.vy = vy;
    this.vz = vz;
    this.mass = mass;
}

function advance(bodies, dt) {
    const size = bodies.length;
    for (let i = 0; i < size; i++) {
        const bi = bodies[i];
        for (let j = i + 1; j < size; j++) {
            const bj = bodies[j];
            const dx = bi.x - bj.x;
            const dy = bi.y - bj.y;
            const dz = bi.z - bj.z;
            const dist = Math.sqrt(dx * dx + dy * dy + dz * dz);
            const mag = dt / (dist * dist * dist);
            bi.vx -= dx * bj.mass * mag;
            bi.vy -= dy * bj.mass * mag;
            bi.vz -= dz * bj.mass * mag;
            bj.vx += dx * bi.mass * mag;
            bj.vy += dy * bi.mass * mag;
            bj.vz += dz * bi.mass * mag;
        }
    }
    for (let i = 0; i < size; i++) {
        const b = bodies[i];
        b.x += dt * b.vx;
        b.y += dt * b.vy;
        b.z += dt * b.vz;
    }
}

const bodies = [
    new Body(0.0, 0.0, 0.0, 0.0, 0.0, 0.0, 1.0),
    new Body(4.84143144246472090e+00, -1.16032004402742839e+00, -1.03622044471123109e-01,
             1.66007664274403694e-03, 7.69901118481134865e-03, -6.90460016972063023e-05,
             9.54791938424326609e-04),
    new Body(8.34336671824457987e+00, 4.12479856412430479e+00, -4.03523417114321381e-01,
             -2.76742510726862411e-03, 4.99852801234917238e-03, 2.30417297573763929e-05,
             2.85885980666130812e-04)
];

for (let i = 0; i < 5000; i++) {
    advance(bodies, 0.01);
}
