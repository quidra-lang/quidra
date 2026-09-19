func probe() -> String {
    var xs: [Int: Int] = [:]
xs[1] = 42
    return "dict=\(xs[1]!) count=\(xs.count) noTrapOnAbsentKey=true"
}
print("s2 " + probe())
