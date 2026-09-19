declare function require(m: string): any;

console.log("ADV-START");

const s: string = require("fs").readFileSync("inputs/ADV-23.bin", "utf8");

console.log("OBS=CP:" + String(s.length));
console.log("ADV-END");
