function probe(cond: boolean): number {
// BEGIN PROBE F02.P1
let v: number
if (cond) v = 5
else v = 9
return v
// END PROBE F02.P1
}

console.log(probe(true))
