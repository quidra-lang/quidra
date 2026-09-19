function probe(): number {
// BEGIN PROBE F08.P1
const m = 2147483647
const o = m + 1 | 0
return o
// END PROBE F08.P1
}

console.log(probe())
