function readUninitialized(): void {
  let v: bigint;
  console.log("OBS=VAL:" + v);
}
console.log("ADV-START");
readUninitialized();
console.log("ADV-END");
