console.log("ADV-START");
const s = new Int32Array(1);
s[0] = -1;
const u = new Uint32Array(1);
u[0] = 1;
const cmp = s[0] < u[0];
console.log("OBS=CMP:" + cmp);
console.log("ADV-END");
