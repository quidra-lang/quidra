struct Money: ExpressibleByIntegerLiteral {
    var cents: Swift.Int
    init(integerLiteral value: Swift.Int) { cents = value }
    init(_ c: Swift.Int) { cents = c }
    static func / (l: Money, r: Money) -> Money { print("user /"); return Money(l.cents * 100) }
    static func % (l: Money, r: Money) -> Money { Money(0) }
    static func + (l: Money, r: Money) -> Money { Money(l.cents + r.cents) }
}
typealias Int32 = Money

func probe() -> Int32 {
// BEGIN PROBE F06.P2
func divmod2(_ a: Int32, _ b: Int32) -> (Int32, Int32) { (a / b, a % b) }
let (q, r) = divmod2(7, 3)
return q + r
// END PROBE F06.P2
}

print(probe().cents)
