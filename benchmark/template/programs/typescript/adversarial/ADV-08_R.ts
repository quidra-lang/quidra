declare function require(m: string): any;
console.log("ADV-START");
const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const a: bigint = BigInt(lines[0]);
const b: bigint = BigInt(lines[1]);
const q = a / b;
console.log("OBS=QUOT:" + q);
console.log("ADV-END");
