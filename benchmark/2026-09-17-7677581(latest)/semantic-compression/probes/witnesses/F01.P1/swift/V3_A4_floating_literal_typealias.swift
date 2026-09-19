typealias IntegerLiteralType = Double
func probe() -> Double {
let n = 7
return n
}
print("type=\(type(of: probe())) value=\(probe())")
