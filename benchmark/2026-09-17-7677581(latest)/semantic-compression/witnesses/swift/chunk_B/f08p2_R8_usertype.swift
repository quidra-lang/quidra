struct M: ExpressibleByIntegerLiteral, Equatable {
    var v: Swift.Int
    init(integerLiteral value: Swift.Int) { v = value }
    static func / (l: M, r: M) -> M { print("user /"); return M(integerLiteral: -42) }
    static func == (l: M, r: M) -> Bool { false }
}
func guarded(_ a: M, _ b: M) -> M {
// BEGIN PROBE F08.P2
let q = b == 0 ? 0 : a / b
return q
// END PROBE F08.P2
}

print(guarded(7, 0).v)
