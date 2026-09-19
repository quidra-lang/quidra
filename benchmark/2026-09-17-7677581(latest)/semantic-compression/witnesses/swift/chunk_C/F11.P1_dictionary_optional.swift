func readAt(_ xs: [Int: String], _ i: Int) -> String? {
let e = xs[i]
return e
}
print(readAt([1: "x"], 999) as Any)
