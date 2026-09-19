console.log("ADV-START");
const src: bigint = 9223372036854775807n;
const dst = new Int32Array(1);
dst[0] = Number(src);
console.log("OBS=V:" + dst[0]);
console.log("ADV-END");
