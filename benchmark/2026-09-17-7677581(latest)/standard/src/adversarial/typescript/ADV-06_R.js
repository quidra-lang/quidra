"use strict";
console.log("ADV-START");
const lines = require("fs").readFileSync(0, "utf8").split("\n");
const a = Number.parseFloat(lines[0]);
const b = Number.parseFloat(lines[1]);
const m = a / b;
const seq = [3.0, m, 1.0];
let best = seq[0];
for (let i = 1; i < seq.length; i++) {
    if (seq[i] > best) {
        best = seq[i];
    }
}
const selfeq = m === m;
console.log("OBS=MAX:" + best.toFixed(6) + "|SELFEQ:" + selfeq);
console.log("ADV-END");
