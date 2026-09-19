func probe(_ a: Int32, _ b: Int32) -> Int32 {
// BEGIN PROBE F08.P2
    let d = a.dividedReportingOverflow(by: b)
    let q = d.overflow ? 0 : d.partialValue
    return q
// END PROBE F08.P2
}

print(probe(7, 0))
