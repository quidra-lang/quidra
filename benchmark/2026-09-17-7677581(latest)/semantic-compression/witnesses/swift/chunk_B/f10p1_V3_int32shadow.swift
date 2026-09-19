typealias Int32 = Int8

func narrow(_ big: Int64) -> Int32 {
// BEGIN PROBE F10.P1
let small = Int32(exactly: big) ?? 0
return small
// END PROBE F10.P1
}

print(narrow(200), narrow(5))
