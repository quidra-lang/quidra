struct M {
    var v: Int
    static func * (l: M, r: M) -> M { print("user *"); return M(v: l.v + r.v) }
    static func + (l: M, r: M) -> M { M(v: l.v * 10) }
}
let a = M(v: 1)
let b = M(v: 2)
let c = M(v: 3)
// BEGIN PROBE F07.P1
let r = a * b + c
// END PROBE F07.P1
print(r.v)
