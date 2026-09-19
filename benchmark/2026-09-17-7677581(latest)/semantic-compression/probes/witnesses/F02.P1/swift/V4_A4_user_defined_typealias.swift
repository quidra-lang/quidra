struct Level: ExpressibleByIntegerLiteral {
    var raw: Int
    init(integerLiteral value: Int) { raw = value }
}
typealias Int32 = Level
func probe(cond: Bool) -> Int32 {
let v: Int32
if cond {
    v = 5
} else {
    v = 9
}
return v
}
print("type=\(type(of: probe(cond: true))) raw=\(probe(cond: true).raw)")
