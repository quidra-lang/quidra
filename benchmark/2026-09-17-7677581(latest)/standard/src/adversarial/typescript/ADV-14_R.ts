declare function require(m: string): any;

function scale(n: bigint): bigint {
    return n * 3n;
}

console.log("ADV-START");

const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const t: string = lines[0];

const r: bigint = scale(t);

console.log("OBS=R:" + String(r));
console.log("ADV-END");
