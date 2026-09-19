typealias UInt8 = Double
func probe() -> UInt8 {
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 16)
buf.initialize(to: 1)
return buf[0]
}
print("type=\(type(of: probe())) value=\(probe()) stride=\(MemoryLayout<UInt8>.stride)")
