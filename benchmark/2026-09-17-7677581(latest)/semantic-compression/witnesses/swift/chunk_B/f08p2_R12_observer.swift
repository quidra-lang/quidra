func guarded(_ a: Int32, _ b: Int32) -> Int32 {
// BEGIN PROBE F08.P2
let q = b == 0 ? 0 : a / b
return q
// END PROBE F08.P2
}

print(guarded(7, 0), guarded(7, 2))
