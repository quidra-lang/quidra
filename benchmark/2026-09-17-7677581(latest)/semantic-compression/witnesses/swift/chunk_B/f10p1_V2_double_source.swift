func narrow(_ big: Double) -> Int32 {
// BEGIN PROBE F10.P1
let small = Int32(exactly: big) ?? 0
return small
// END PROBE F10.P1
}

print(narrow(2.5), narrow(Double.nan), narrow(5.0))
