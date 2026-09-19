"use strict";
function readUninitialized() {
    let v;
    console.log("OBS=VAL:" + v);
}
console.log("ADV-START");
readUninitialized();
console.log("ADV-END");
