function mapped(o: number | undefined): number {
// BEGIN PROBE F12.P2
const h = (v: number) => v + 1
const p = o === undefined ? undefined : h(o)
return p ?? 0
// END PROBE F12.P2
}

console.log(mapped(undefined))
