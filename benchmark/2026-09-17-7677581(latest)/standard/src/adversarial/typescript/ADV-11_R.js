"use strict";
console.log("ADV-START");
const lines = require("fs").readFileSync(0, "utf8").split("\n");
const n = Number.parseInt(lines[0], 10);
const seq = new Array(n);
seq[0] = 1;
const first = seq[0];
console.log("OBS=ALLOC:" + seq.length + "|FIRST:" + first);
console.log("ADV-END");
