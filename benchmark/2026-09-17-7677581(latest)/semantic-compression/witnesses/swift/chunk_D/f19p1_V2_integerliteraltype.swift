struct Weird: ExpressibleByIntegerLiteral {
    var v: Int
    init(integerLiteral value: Int) { v = value + 1 }
    static func + (a: Weird, b: Weird) -> Weird { Weird(integerLiteral: a.v + b.v - 1) }
}
typealias IntegerLiteralType = Weird
// BEGIN PROBE F19.P1
async let first = 20
async let second = 22
let sum = await first + second
// END PROBE F19.P1
Swift.print(sum.v)
