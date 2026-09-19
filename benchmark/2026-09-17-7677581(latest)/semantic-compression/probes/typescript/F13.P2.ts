function parseOrZero(s: string): number {
// BEGIN PROBE F13.P2
const p = parseInt(s, 10)
const n = Number.isNaN(p) ? 0 : p
return n
// END PROBE F13.P2
}

console.log(parseOrZero("21"))
