"use strict";
// DBG-1
class Item {
    constructor(count, name) {
        this.count = count;
        this.name = name;
    }
}
const ITERATIONS = 40000000;
const MODULUS = 1000000007;
function accumulate(items, iterations) {
    let total = 0;
    for (let i = 0; i < iterations; i++) {
        for (const item of items) {
            total = (total * 31 + item.count + item.name.length) % MODULUS;
        }
    }
    return total;
}
const items = [];
items.push(new Item(7, "alpha"));
items.push(new Item(11, "bravo"));
items.push(new Item(13, "charlie"));
const checksum = accumulate(items, ITERATIONS);
console.log("checksum=" + checksum);
//# sourceMappingURL=dbg1.js.map