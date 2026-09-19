func probe() -> UInt8 {
// BEGIN PROBE F02.P2
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 16)
buf.initialize(to: 1)
return buf[0]
// END PROBE F02.P2
}

print(probe())
