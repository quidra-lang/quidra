function probe(): number {
// BEGIN PROBE F06.P1
const mid = (s: number[]) => s[1]
const xs = [1, 2, 3]
const y = mid(xs)
return y
// END PROBE F06.P1
}

console.log(probe())
