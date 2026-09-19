function f(): number {
// BEGIN PROBE F16.P1
const xs = new Int32Array([1, 2, 3]);
const total = xs.reduce((a, b) => a + b, 0);
return total;
// END PROBE F16.P1
}
console.log(f());
