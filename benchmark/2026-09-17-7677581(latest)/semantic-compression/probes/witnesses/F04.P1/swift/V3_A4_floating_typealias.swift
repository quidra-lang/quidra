typealias Int32 = Double
func probe() -> Int32 {
var slot: Int32 = 4
withUnsafeMutablePointer(to: &slot) { port in
    port.pointee = 9
}
return slot
}
print("type=\(type(of: probe())) value=\(probe())")
