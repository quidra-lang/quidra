struct Money: ExpressibleByIntegerLiteral {
    var raw: Int
    init(integerLiteral value: Int) { raw = value }
}
typealias IntegerLiteralType = Money
func probe() -> Money {
let n = 7
return n
}
print("type=\(type(of: probe())) raw=\(probe().raw)")
