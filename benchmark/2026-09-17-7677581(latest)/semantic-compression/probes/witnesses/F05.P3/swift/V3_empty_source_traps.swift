func probe() -> Int32 {
    let xs: [Int32] = []
    let k: Int32 = 0
let neg = { (a: Int32) in k - a }
let ys = xs.map(neg)
return ys[0]
}
print(probe())
