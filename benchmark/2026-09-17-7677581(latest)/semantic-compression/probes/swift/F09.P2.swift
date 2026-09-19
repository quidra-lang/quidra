func probe(_ a: Int32, _ b: Int32) -> ([Int], Bool) {
    var xs = [3, 1, 2]
// BEGIN PROBE F09.P2
    xs.sort(by: >)
    let lt = a < b
// END PROBE F09.P2
    return (xs, lt)
}

let out = probe(1, 2)
print(out.0, out.1)
