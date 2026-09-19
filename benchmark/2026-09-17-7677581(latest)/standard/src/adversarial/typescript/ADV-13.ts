class A {
  constructor(public v: bigint) {}
}
class B {
  constructor(public v: bigint) {}
}
console.log("ADV-START");
const a = new A(42n);
const u: unknown = a;
const b = u as B;
console.log("OBS=V:" + b.v);
console.log("ADV-END");
