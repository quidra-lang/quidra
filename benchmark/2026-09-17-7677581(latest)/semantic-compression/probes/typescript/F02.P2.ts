function probe(): number {
// BEGIN PROBE F02.P2
const buf = new Uint8Array(16)
buf[0] = 1
return buf[0]
// END PROBE F02.P2
}

console.log(probe())
