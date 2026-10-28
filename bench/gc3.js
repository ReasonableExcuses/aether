// Binary trees benchmark (classic GC stress probe)
function TreeNode(left, right, item) {
    this.left = left;
    this.right = right;
    this.item = item;
}

function bottomUpTree(item, depth) {
    if (depth > 0) {
        return new TreeNode(
            bottomUpTree(2 * item - 1, depth - 1),
            bottomUpTree(2 * item, depth - 1),
            item
        );
    }
    return new TreeNode(null, null, item);
}

function itemCheck(node) {
    if (!node.left) return node.item;
    return node.item + itemCheck(node.left) - itemCheck(node.right);
}

const maxDepth = 8;
const stretchDepth = maxDepth + 1;
let check = itemCheck(bottomUpTree(0, stretchDepth));
const longLivedTree = bottomUpTree(0, maxDepth);

for (let depth = 4; depth <= maxDepth; depth += 2) {
    const iterations = 1 << (maxDepth - depth + 2);
    let total = 0;
    for (let i = 1; i <= iterations; i++) {
        total += itemCheck(bottomUpTree(i, depth));
        total += itemCheck(bottomUpTree(-i, depth));
    }
}

let finalCheck = itemCheck(longLivedTree);
if (typeof finalCheck !== "number") {
    throw new Error("GC3 assertion failed");
}
