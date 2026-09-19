function subRange(xs: number[]): number {
// BEGIN PROBE F11.P2
const part = xs.slice(1, 4)
return part[0]
// END PROBE F11.P2
}

console.log(subRange([10, 20, 30, 40, 50]))
