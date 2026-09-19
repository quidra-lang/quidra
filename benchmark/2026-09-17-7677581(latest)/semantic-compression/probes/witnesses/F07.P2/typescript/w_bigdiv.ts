function f(a: bigint, b: bigint): [bigint, bigint] { return [a / b, a % b] }
console.log("7n/2n:", f(7n, 2n).join(","))
console.log("-7n/2n:", f(-7n, 2n).join(","))
try { f(1n, 0n) } catch (e) { console.log("THREW:", (e as Error).constructor.name, (e as Error).message) }
const m2 = 2147483647
console.log("plain m+1:", m2 + 1, "| wrapped m+1|0:", m2 + 1 | 0)
console.log("ToInt32 of 1e10:", 1e10 | 0)
