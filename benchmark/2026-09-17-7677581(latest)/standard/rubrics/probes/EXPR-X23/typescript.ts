const buf = new Int32Array(4);
const view = buf.subarray(1, 3);   // non-copying view onto the same ArrayBuffer
view[0] = 99; view[1] = 99;
console.log("X23", buf[1], buf[2]);
