console.log("ADV-START");
function readUninitialized(): void {
  let v: bigint;
  console.log("OBS=VAL:" + v);
}
readUninitialized();
console.log("ADV-END");
