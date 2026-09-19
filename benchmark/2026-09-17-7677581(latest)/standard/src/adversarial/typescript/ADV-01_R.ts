declare function require(m: string): any;
console.log("ADV-START");
const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const src: bigint = BigInt(lines[0]);
const dst = new Int32Array(1);
dst[0] = Number(src);
console.log("OBS=V:" + dst[0]);
console.log("ADV-END");
