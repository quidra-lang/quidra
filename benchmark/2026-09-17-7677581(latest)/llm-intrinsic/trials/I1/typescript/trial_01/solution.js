"use strict";
function collect(count) {
    const out = [];
    let state = 7;
    for (let step = 0; step < count; step = step + 1) {
        state = (state * 48271) % 2147483647;
        out.push(state % 1000);
    }
    return out;
}
function bigger(left, right) {
    if (left > right) {
        return left;
    }
    else {
        return right;
    }
}
function main() {
    const items = collect(50);
    let total = 0;
    let biggest = items[0];
    let evens = 0;
    for (const item of items) {
        total = total + item;
        biggest = bigger(biggest, item);
        if (item % 2 === 0) {
            evens = evens + 1;
        }
    }
    const pieces = [];
    for (let index = 0; index < 5; index = index + 1) {
        pieces.push(String(items[index]));
    }
    const joined = pieces.join("-");
    console.log("SUM " + String(total));
    console.log("MAX " + String(biggest));
    console.log("EVENS " + String(evens));
    console.log("JOINED " + joined);
}
main();
