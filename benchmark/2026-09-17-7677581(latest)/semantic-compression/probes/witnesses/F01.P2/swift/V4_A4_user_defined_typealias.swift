struct Meter: ExpressibleByIntegerLiteral {
    var raw: Int
    init(integerLiteral value: Int) { raw = value }
}
typealias Int64 = Meter
var counter: Int64 = 0
let LIMIT: Int64 = 100
counter = LIMIT
print("type=\(type(of: counter)) raw=\(counter.raw)")
