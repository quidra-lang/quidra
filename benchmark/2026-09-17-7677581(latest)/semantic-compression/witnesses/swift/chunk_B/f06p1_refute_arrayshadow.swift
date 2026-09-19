struct MyArr: ExpressibleByArrayLiteral {
    var e: [Swift.Int]
    init(arrayLiteral elements: Swift.Int...) { e = elements }
    subscript(i: Swift.Int) -> Swift.Int { e[i] * 10 }
}
typealias Array = MyArr

func probe() -> Int {
// BEGIN PROBE F06.P1
func mid(_ v: [Int]) -> Int { v[1] }
let xs = [1, 2, 3]
let y = mid(xs)
return y
// END PROBE F06.P1
}

print(probe())
