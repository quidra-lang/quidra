declare function require(m: string): any;

console.log("ADV-START");

const lines: string[] = require("fs").readFileSync(0, "utf8").split("\n");
const t: string = lines[0];

const n: number = Number.parseInt(t, 10);
const r: number = n * 2;

console.log("OBS=R:" + String(r));
console.log("ADV-END");
