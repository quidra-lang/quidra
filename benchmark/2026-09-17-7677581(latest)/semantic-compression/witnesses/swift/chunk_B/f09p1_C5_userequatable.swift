struct P: Equatable {
    var v: Int
    static func == (l: P, r: P) -> Bool { print("user =="); return l.v != r.v }
}
let s1 = P(v: 1)
let s2 = P(v: 1)
// BEGIN PROBE F09.P1
let eq = s1 == s2
// END PROBE F09.P1
print(eq)
