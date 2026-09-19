// DBG-1
class Item {
    count: number;
    name: string;
    constructor(count: number, name: string) {
        this.count = count;
        this.name = name;
    }
}

const ITERATIONS: number = 40000000;
const MODULUS: number = 1000000007;

function accumulate(items: Item[], iterations: number): number {
    let total: number = 0;
    for (let i = 0; i < iterations; i++) {
        for (const item of items) {
            total = (total * 31 + item.count + item.name.length) % MODULUS;
        }
    }
    return total;
}

const items: Item[] = [];
items.push(new Item(7, "alpha"));
items.push(new Item(11, "bravo"));
items.push(new Item(13, "charlie"));
const checksum: number = accumulate(items, ITERATIONS);
console.log("checksum=" + checksum);
