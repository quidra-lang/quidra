struct Flag: ExpressibleByIntegerLiteral {
    var raw: Int
    init(integerLiteral value: Int) { raw = value }
}
typealias UInt8 = Flag
func probe() -> UInt8 {
let buf = UnsafeMutablePointer<UInt8>.allocate(capacity: 16)
buf.initialize(to: 1)
return buf[0]
}
print("type=\(type(of: probe())) raw=\(probe().raw) stride=\(MemoryLayout<UInt8>.stride)")
