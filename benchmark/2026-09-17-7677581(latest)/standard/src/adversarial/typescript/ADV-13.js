"use strict";
class A {
    v;
    constructor(v) {
        this.v = v;
    }
}
class B {
    v;
    constructor(v) {
        this.v = v;
    }
}
console.log("ADV-START");
const a = new A(42n);
const u = a;
const b = u;
console.log("OBS=V:" + b.v);
console.log("ADV-END");
