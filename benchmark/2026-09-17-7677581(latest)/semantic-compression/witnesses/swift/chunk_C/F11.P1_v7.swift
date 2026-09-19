func readAt(_ xs: [UnsafeMutablePointer<Int>], _ i: Int) -> UnsafeMutablePointer<Int> {
let e = xs[i]
return e
}
let p = UnsafeMutablePointer<Int>.allocate(capacity: 1)
p.initialize(to: 5)
print(readAt([p, p, p], 2).pointee)
