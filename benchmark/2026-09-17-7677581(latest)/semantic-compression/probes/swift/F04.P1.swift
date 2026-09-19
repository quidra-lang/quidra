func probe() -> Int32 {
// BEGIN PROBE F04.P1
var slot: Int32 = 4
withUnsafeMutablePointer(to: &slot) { port in
    port.pointee = 9
}
return slot
// END PROBE F04.P1
}

print(probe())
