let s1 = "e\u{0301}"
let s2 = "\u{00E9}"
// BEGIN PROBE F09.P1
let eq = s1 == s2
// END PROBE F09.P1
print(eq, s1.unicodeScalars.count, s2.unicodeScalars.count)
