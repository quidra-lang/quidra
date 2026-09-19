func probe() -> Int {
    let xs = [7, 8, 9]
// BEGIN PROBE F04.P2
let window = xs[...]
return window[0]
// END PROBE F04.P2
}

print(probe())
