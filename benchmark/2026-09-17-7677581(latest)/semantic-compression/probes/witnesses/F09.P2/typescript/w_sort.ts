const xs1: number[] = [3, 1, 2]
const r1 = xs1.sort((p, q) => q - p)
console.log("in place:", xs1.join(","), "same object:", r1 === xs1)
const xs2: any = { sort(f: any) { return [9, 8] } }
const before = JSON.stringify(xs2)
xs2.sort((p: any, q: any) => q - p)
console.log("user sort, receiver unchanged:", JSON.stringify(xs2) === before)
const xs3: any = [{ valueOf() { throw new Error("cmp") } }, 1, 2]
try { xs3.sort((p: any, q: any) => q - p) } catch (e) { console.log("THREW:", (e as Error).message, "| xs3 len", xs3.length) }
const ao: any = { valueOf() { (globalThis as any).hit = true; return 1 } }
console.log("user < :", ao < 2, "side effect:", (globalThis as any).hit)
