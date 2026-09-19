func narrow(_ big: Int16) -> Int32 {
// BEGIN PROBE F10.P1
let small = Int32(exactly: big) ?? 0
return small
// END PROBE F10.P1
}

print(narrow(Int16.min), narrow(Int16.max))
