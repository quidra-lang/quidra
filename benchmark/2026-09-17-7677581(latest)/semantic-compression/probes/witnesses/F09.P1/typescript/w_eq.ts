function eqp(s1: string, s2: string): boolean { const eq = s1 === s2; return eq }
function eqo(s1: String, s2: String): boolean { const eq = s1 === s2; return eq }
function eqn(s1: number, s2: number): boolean { const eq = s1 === s2; return eq }
function eqb(s1: bigint, s2: bigint): boolean { const eq = s1 === s2; return eq }
console.log("string prims:", eqp(["ab","cd"].join(""), "ab".concat("cd")))
console.log("String objects:", eqo(new String("abcd"), new String("abcd")))
console.log("numbers NaN:", eqn(NaN, NaN), "zeros:", eqn(0, -0))
console.log("bigints:", eqb(2n ** 70n, 2n ** 70n))
