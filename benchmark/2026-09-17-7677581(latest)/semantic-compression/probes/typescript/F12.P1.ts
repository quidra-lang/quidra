function fallback(): number {
// BEGIN PROBE F12.P1
const o: number | undefined = undefined
const n = o ?? 0
return n
// END PROBE F12.P1
}

console.log(fallback())
