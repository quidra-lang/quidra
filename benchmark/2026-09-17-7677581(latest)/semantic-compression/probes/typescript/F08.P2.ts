function probe(a: number, b: number): number {
// BEGIN PROBE F08.P2
const q = b === 0 ? 0 : Math.trunc(a / b)
return q
// END PROBE F08.P2
}

console.log(probe(-7, 0), probe(-7, 2))
