struct T: Comparable {
    var v: Int
    static func < (l: T, r: T) -> Bool { fatalError("user <") }
}
var xs = [T(v: 3), T(v: 1), T(v: 2)]
let a: Int32 = 1
let b: Int32 = 2
// BEGIN PROBE F09.P2
xs.sort(by: >)
let lt = a < b
// END PROBE F09.P2
print(xs.map(\.v), lt)
