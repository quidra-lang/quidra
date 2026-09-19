function probe(a: number, b: number): string {
const q = Math.trunc(a / b)
const m = a % b
const d = a / b
return q + " " + m + " " + d
}
console.log("(-7,2):", probe(-7, 2))
console.log("(7,-2):", probe(7, -2))
console.log("(-7,0):", probe(-7, 0))
console.log("(0,0):", probe(0, 0))
function probeA(a: any, b: any): any { const q = Math.trunc(a / b); const m = a % b; const d = a / b; return [q, m, d] }
console.log("bigint:", (() => { try { return probeA(7n, 2n) } catch (e) { return "THREW: " + (e as Error).message } })())
console.log("string any:", probeA("7", "2"))
