func readAt(_ xs: [UInt64], _ i: Int) -> UInt64 {
let e = xs[i]
return e
}
print(readAt([1, 2, 18446744073709551615], 2))
