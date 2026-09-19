function combine(): string {
// BEGIN PROBE F15.P1
function head<T>(xs: T[]): T {
return xs[0]
}
const a = head([4, 5, 6])
const b = head(["p", "q"])
return String(a) + b
// END PROBE F15.P1
}

console.log(combine())
