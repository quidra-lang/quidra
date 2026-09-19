const xs: number[] = [3, 1, 2]
const a: number = 1
const b: number = 2
// BEGIN PROBE F09.P2
xs.sort((p, q) => q - p)
const lt = a < b
// END PROBE F09.P2
console.log(xs.join(","), lt)
