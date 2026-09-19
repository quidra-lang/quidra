func probe() -> Int32 {
// BEGIN PROBE F08.P1
    let m = Int32.max
    let s = m.addingReportingOverflow(1)
    let o = s.overflow ? 0 : s.partialValue
    return o
// END PROBE F08.P1
}

print(probe())
