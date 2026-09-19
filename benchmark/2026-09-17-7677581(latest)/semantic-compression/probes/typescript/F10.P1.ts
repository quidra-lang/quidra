function probe(big: bigint): bigint {
// BEGIN PROBE F10.P1
const small = BigInt.asIntN(32, big)
return small
// END PROBE F10.P1
}

console.log(probe(2147483648n))
