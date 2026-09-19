const big: bigint = 9007199254740993n
console.log("asIntN:", BigInt.asIntN(32, big), "NumberBitOr:", Number(big) | 0)
const a: number = -2147483648
const b: number = -1
console.log("trunc:", Math.trunc(a / b), "bitor:", a / b | 0)
