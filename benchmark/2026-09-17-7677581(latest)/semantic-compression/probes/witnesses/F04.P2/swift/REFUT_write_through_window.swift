func probe() -> Int {
    let xs = [7, 8, 9]
    let window = xs[...]
    window[0] = 1
    return window[0]
}
print(probe())
