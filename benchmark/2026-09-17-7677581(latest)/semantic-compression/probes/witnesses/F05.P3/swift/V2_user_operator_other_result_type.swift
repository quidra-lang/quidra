struct K {}
func - (lhs: K, rhs: Int32) -> String { "neg\(rhs)" }
func probe() -> String {
    let xs: [Int32] = [1, 2, 3]
    let k = K()
let neg = { (a: Int32) in k - a }
let ys = xs.map(neg)
return ys[0]
}
print("v2 result=\(probe()) type=\(type(of: probe()))")
