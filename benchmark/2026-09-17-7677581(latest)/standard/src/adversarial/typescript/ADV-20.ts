declare function require(m: string): any;

function f(n: bigint): bigint {
    if (n === 1000000n) {
        return 0n;
    }
    return 1n + f(n + 1n);
}

console.log("ADV-START");

const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const r: bigint = f(BigInt(lines[0]));

console.log("OBS=R:" + String(r));
console.log("ADV-END");
