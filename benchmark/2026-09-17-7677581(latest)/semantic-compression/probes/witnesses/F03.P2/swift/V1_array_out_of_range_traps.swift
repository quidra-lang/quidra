func probe() -> Int {
    var xs = [7]
xs[1] = 42
    return xs[1]
}
print(probe())
