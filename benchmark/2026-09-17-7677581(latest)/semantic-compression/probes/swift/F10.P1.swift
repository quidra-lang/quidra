func probe(_ big: Int64) -> Int32 {
// BEGIN PROBE F10.P1
    let small = Int32(exactly: big) ?? 0
    return small
// END PROBE F10.P1
}

print(probe(2147483648))
