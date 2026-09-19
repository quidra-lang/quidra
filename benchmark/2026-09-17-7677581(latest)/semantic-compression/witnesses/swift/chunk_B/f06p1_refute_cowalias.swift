func probe() -> (Int, [Int]) {
var xs = [1, 2, 3]
func mid(_ v: [Int]) -> Int { v[1] }
let y = mid(xs)
xs[1] = 99
return (y, xs)
}

print(probe())
