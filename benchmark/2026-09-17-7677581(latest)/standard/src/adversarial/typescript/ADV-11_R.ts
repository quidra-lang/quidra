declare function require(m: string): any;
console.log("ADV-START");
const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const n: number = Number.parseInt(lines[0], 10);
const seq: number[] = new Array<number>(n);
seq[0] = 1;
const first = seq[0];
console.log("OBS=ALLOC:" + seq.length + "|FIRST:" + first);
console.log("ADV-END");
