func probe() -> Int32 {
    let xs: [Int32] = [1, 2, 3]
    let k: Int32 = 0
// BEGIN PROBE F05.P3
let neg = { (a: Int32) in k - a }
let ys = xs.map(neg)
return ys[0]
// END PROBE F05.P3
}

print(probe())
