struct Port: ExpressibleByIntegerLiteral {
    var raw: Int
    init(integerLiteral value: Int) { raw = value }
}
typealias Int32 = Port
func probe() -> Int32 {
var slot: Int32 = 4
withUnsafeMutablePointer(to: &slot) { port in
    port.pointee = 9
}
return slot
}
print("type=\(type(of: probe())) raw=\(probe().raw)")
