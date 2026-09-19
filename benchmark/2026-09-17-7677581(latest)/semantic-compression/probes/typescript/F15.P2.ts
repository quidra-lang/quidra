function greater(): number {
// BEGIN PROBE F15.P2
function maxOf<T extends number | string>(a: T, b: T): T {
return a < b ? b : a
}
const m = maxOf(3, 5)
return m
// END PROBE F15.P2
}

console.log(greater())
