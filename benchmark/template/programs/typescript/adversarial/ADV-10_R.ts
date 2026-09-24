declare function require(m: string): any;
console.log("ADV-START");
const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const seq: number[] = [10, 20, 30, 40, 50];
const i: number = Number.parseInt(lines[0], 10);
const elem = seq[i];
console.log("OBS=ELEM:" + elem);
console.log("ADV-END");
