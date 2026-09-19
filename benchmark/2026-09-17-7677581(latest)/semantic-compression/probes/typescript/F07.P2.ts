function probe(a: number, b: number): string {
// BEGIN PROBE F07.P2
const q = Math.trunc(a / b)
const m = a % b
const d = a / b
// END PROBE F07.P2
return q + " " + m + " " + d
}

console.log(probe(-7, 2))
