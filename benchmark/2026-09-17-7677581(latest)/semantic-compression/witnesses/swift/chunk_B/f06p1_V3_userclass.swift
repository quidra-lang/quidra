final class Box: ExpressibleByIntegerLiteral {
    let v: Swift.Int
    init(integerLiteral value: Swift.Int) { print("alloc \(value)"); self.v = value }
    deinit { print("deinit \(v)") }
}
typealias Int = Box
typealias IntegerLiteralType = Box

func probe() -> Int {
// BEGIN PROBE F06.P1
func mid(_ v: [Int]) -> Int { v[1] }
let xs = [1, 2, 3]
let y = mid(xs)
return y
// END PROBE F06.P1
}

print(probe().v)
