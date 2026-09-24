declare function require(m: string): any;

console.log("ADV-START");

const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const xs: number[] = [];
for (let i: number = 0; i < 7; i++) {
    xs.push(Number(lines[i]));
}
const target: number = Number(lines[7]);

let lo: number = 0;
let hi: number = 6;
let result: number = -1;
while (lo <= hi) {
    const mid: number = Math.floor((lo + hi) / 2);
    if (xs[mid] === target) {
        result = mid;
        break;
    }
    if (xs[mid] < target) {
        lo = mid + 1;
    } else {
        hi = mid - 1;
    }
}

console.log("OBS=IDX:" + String(result));
console.log("ADV-END");
