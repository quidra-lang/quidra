function readAtChecked(xs: number[], i: number): number {
// BEGIN PROBE F11.P1.checked
const e = xs.at(i) ?? 0
return e
// END PROBE F11.P1.checked
}

console.log(readAtChecked([1, 2, 3], 2))
