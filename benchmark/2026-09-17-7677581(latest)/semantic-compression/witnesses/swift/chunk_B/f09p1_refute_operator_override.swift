func == (lhs: String, rhs: String) -> Bool { return false }
let s1 = "abcdefghijklmnopqrst"
let s2 = "abcdefghij" + "klmnopqrst"
// BEGIN PROBE F09.P1
let eq = s1 == s2
// END PROBE F09.P1
print(eq)
