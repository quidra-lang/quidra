func probe() -> String {
    var xs = [7, 8, 9]
xs[1] = 42
    var small = [0]
    return "arr=\(xs[1]) count=\(xs.count) oobWouldTrap=true small=\(small.count)"
}
print("s1 " + probe())
