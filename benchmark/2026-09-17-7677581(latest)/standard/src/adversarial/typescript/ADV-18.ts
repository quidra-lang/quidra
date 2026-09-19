declare function require(m: string): any;

function pick(b: boolean): bigint {
    if (b) {
        return 1n;
    }
}

console.log("ADV-START");

const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const b: boolean = lines[0] === "1";

const r: bigint = pick(b) * 2n;

console.log("OBS=R:" + String(r));
console.log("ADV-END");
