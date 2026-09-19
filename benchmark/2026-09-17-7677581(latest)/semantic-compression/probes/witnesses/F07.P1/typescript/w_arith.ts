function probeN(a: number, b: number, c: number): number { const r = a * b + c; return r }
function probeB(a: bigint, b: bigint, c: bigint): bigint { const r = a * b + c; return r }
function probeA(a: any, b: any, c: any): any { const r = a * b + c; return r }
console.log("number:", probeN(7, 6, 5))
console.log("number overflow:", probeN(1e308, 10, 0))
console.log("number rounding:", probeN(0.1, 3, 0))
console.log("bigint exact:", probeB(2n ** 62n, 2n ** 62n, 1n))
console.log("any string coercion:", probeA("2", "3", "!"))
try { console.log(probeA(1, 2n, 3)) } catch (e) { console.log("THREW:", (e as Error).constructor.name, (e as Error).message) }
const o = { valueOf() { (globalThis as any).sideEffect = true; return 3 } }
console.log("any user valueOf:", probeA(o, 2, 1), "global written:", (globalThis as any).sideEffect)
