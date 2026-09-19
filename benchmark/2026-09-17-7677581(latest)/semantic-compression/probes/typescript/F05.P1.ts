function f(v: number[]): void {
  v[0] = 99
}

function probe(): number {
// BEGIN PROBE F05.P1
const x = [1, 2, 3]
f(x)
return x[0]
// END PROBE F05.P1
}

console.log(probe())
