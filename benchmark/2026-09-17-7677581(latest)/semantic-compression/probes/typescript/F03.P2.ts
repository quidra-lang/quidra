function probe(): number {
const xs: number[] = [1, 2, 3]
// BEGIN PROBE F03.P2
xs[1] = 42
// END PROBE F03.P2
return xs[1]
}

console.log(probe())
