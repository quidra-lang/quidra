typealias IntegerLiteralType = UInt8
func probe() -> UInt8 {
let n = 7
return n
}
print("type=\(type(of: probe())) value=\(probe())")
