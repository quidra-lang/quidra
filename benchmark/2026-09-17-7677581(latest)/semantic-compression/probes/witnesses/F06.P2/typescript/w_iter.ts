Object.defineProperty(Array.prototype, Symbol.iterator, {
  value: function* () { throw new Error("patched iterator") },
  writable: true, configurable: true
})
function probe(): number {
const divmod2 = (a: number, b: number): [number, number] => [Math.trunc(a / b), a % b]
const [q, r] = divmod2(7, 3)
return q + r
}
try { console.log(probe()) } catch (e) { console.log("THREW:", (e as Error).message) }
