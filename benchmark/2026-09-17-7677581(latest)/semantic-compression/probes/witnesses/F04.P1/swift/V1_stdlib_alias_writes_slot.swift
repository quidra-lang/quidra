func probe() -> Int32 {
var slot: Int32 = 4
withUnsafeMutablePointer(to: &slot) { port in
    port.pointee = 9
}
return slot
}
print("value=\(probe()) type=\(type(of: probe()))")
