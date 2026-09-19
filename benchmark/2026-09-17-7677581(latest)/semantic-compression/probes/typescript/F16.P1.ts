function sequenceTotal(): number {
// BEGIN PROBE F16.P1
const xs = [1, 2, 3];
const total = xs.reduce((a, b) => a + b);
return total;
// END PROBE F16.P1
}

console.log(sequenceTotal());
