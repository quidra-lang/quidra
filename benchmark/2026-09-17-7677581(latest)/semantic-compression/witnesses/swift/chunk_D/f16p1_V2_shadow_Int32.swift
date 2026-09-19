struct Int32: ExpressibleByIntegerLiteral {
    var v: Int
    init(integerLiteral value: Int) { v = value }
    static func + (a: Int32, b: Int32) -> Int32 { Int32(integerLiteral: a.v * b.v) }
}
func sumSequence() -> Int32 {
// BEGIN PROBE F16.P1
let xs: [Int32] = [1, 2, 3]
let total = xs.reduce(0, +)
return total
// END PROBE F16.P1
}
Swift.print(sumSequence().v)
