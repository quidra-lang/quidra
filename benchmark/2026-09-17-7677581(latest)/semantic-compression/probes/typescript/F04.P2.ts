function probe(): number {
const xs: number[] = [1, 2, 3]
// BEGIN PROBE F04.P2
const window: readonly number[] = xs
return window[0]
// END PROBE F04.P2
}

console.log(probe())
