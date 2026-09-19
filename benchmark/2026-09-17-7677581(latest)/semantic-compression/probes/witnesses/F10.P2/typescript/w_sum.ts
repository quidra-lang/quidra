function s(i: any, d: any): any { const sum = i + d; return sum }
console.log("num+num:", s(3, 0.5), "| 0.1+0.2:", s(0.1, 0.2), "| overflow:", s(1e308, 1e308))
console.log("bigint:", s(2n ** 70n, 1n))
console.log("string:", s("ab", 1), typeof s("ab", 1))
const o = { valueOf() { (globalThis as any).h2 = true; return 4 } }
console.log("object valueOf:", s(o, 1), "side effect:", (globalThis as any).h2)
try { console.log(s(1, 2n)) } catch (e) { console.log("THREW:", (e as Error).message) }
