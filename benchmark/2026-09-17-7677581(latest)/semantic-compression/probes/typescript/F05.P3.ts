function probe(): number {
const xs: number[] = [1, 2, 3]
const k = 0
// BEGIN PROBE F05.P3
const neg = (a: number) => k - a
const ys = xs.map(neg)
return ys[0]
// END PROBE F05.P3
}

console.log(probe())
