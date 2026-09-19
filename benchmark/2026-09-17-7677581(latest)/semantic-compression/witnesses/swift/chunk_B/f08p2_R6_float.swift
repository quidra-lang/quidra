func guarded(_ a: Double, _ b: Double) -> Double {
// BEGIN PROBE F08.P2
let q = b == 0 ? 0 : a / b
return q
// END PROBE F08.P2
}

print(guarded(7, 0), guarded(7, Double.nan))
