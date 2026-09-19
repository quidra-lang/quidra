struct S: Comparable {
    var v: Int
    static func < (l: S, r: S) -> Bool { print("user <"); return l.v > r.v }
}
var xs = [3, 1, 2]
let a = S(v: 1)
let b = S(v: 2)
// BEGIN PROBE F09.P2
xs.sort(by: >)
let lt = a < b
// END PROBE F09.P2
print(xs, lt)
