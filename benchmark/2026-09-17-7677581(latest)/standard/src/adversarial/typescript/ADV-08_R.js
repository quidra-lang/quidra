"use strict";
console.log("ADV-START");
const lines = require("fs").readFileSync(0, "utf8").split("\n");
const a = BigInt(lines[0]);
const b = BigInt(lines[1]);
const q = a / b;
console.log("OBS=QUOT:" + q);
console.log("ADV-END");
