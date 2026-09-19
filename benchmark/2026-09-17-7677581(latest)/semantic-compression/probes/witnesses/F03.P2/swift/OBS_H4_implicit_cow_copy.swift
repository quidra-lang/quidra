func probe() -> String {
    var xs = [7, 8, 9]
    let ys = xs
xs[1] = 42
    return "xs1=\(xs[1]) ys1=\(ys[1]) copyHappened=\(xs[1] != ys[1])"
}
print("cow " + probe())
