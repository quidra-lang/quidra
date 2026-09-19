"use strict";
console.log("ADV-START");
const lines = require("fs").readFileSync(0, "utf8").split("\n");
const f = Number.parseFloat(lines[0]);
const dst = new Int32Array(1);
dst[0] = f;
console.log("OBS=V:" + dst[0]);
console.log("ADV-END");
