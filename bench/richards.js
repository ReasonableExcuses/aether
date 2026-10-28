// Richards benchmark (clean compact subset)
function Task(id, priority, queue) {
    this.id = id;
    this.priority = priority;
    this.queue = queue;
    this.link = null;
    this.state = 0;
}

Task.prototype.run = function(packet) {
    if (packet) {
        packet.link = this.queue;
        this.queue = packet;
    }
    return null;
};

function Packet(link, id, kind) {
    this.link = link;
    this.id = id;
    this.kind = kind;
    this.a1 = 0;
    this.a2 = [];
}

let count = 0;
for (let i = 0; i < 20000; i++) {
    const t = new Task(i, i % 5, null);
    const p = new Packet(null, i, 1);
    t.run(p);
    count += t.priority;
}
if (count === 0) {
    throw new Error("Richards benchmark failed");
}
