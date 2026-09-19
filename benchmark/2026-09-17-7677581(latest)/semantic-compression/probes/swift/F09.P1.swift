func probe(_ s1: String, _ s2: String) -> Bool {
// BEGIN PROBE F09.P1
    let eq = s1 == s2
// END PROBE F09.P1
    return eq
}

let base = "abcdefghij"
print(probe(base + "klmnopqrst", base + "klmnopqrst"))
