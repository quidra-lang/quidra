"use strict";
console.log("ADV-START");
const lines = require("fs").readFileSync(0, "utf8").split("\n");
const a = Number.parseInt(lines[0], 10);
const diff = a - 3;
console.log("OBS=V:" + diff);
console.log("ADV-END");
