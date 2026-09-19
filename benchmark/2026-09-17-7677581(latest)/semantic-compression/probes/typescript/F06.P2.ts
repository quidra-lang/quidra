function probe(): number {
// BEGIN PROBE F06.P2
const divmod2 = (a: number, b: number): [number, number] => [Math.trunc(a / b), a % b]
const [q, r] = divmod2(7, 3)
return q + r
// END PROBE F06.P2
}

console.log(probe())
