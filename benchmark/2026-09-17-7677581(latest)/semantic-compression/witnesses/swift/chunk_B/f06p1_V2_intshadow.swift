typealias Int = UInt8
typealias IntegerLiteralType = UInt8

func probe() -> Int {
// BEGIN PROBE F06.P1
func mid(_ v: [Int]) -> Int { v[1] }
let xs = [1, 2, 3]
let y = mid(xs)
return y
// END PROBE F06.P1
}

print(probe(), probe() &- 3)
