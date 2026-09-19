struct M: ExpressibleByIntegerLiteral {
    var v: Swift.Int
    init(integerLiteral value: Swift.Int) { v = value }
    static func / (l: M, r: M) -> M { print("user /"); return M(integerLiteral: -99) }
    static func % (l: M, r: M) -> M { M(integerLiteral: -98) }
}
extension Double { init(_ m: M) { self = Swift.Double(m.v) * 1000 } }
let a: M = -7
let b: M = 2
// BEGIN PROBE F07.P2
let q = a / b
let m = a % b
let d = Double(a) / Double(b)
// END PROBE F07.P2
print(q.v, m.v, d)
