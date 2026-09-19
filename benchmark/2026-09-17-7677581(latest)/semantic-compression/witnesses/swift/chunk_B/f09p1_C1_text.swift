func bufferAddress(_ s: String) -> UInt {
    var t = s
    return t.withUTF8 { UInt(bitPattern: $0.baseAddress) }
}

let base = "abcdefghij"
let s1 = base + "klmnopqrst"
let s2 = base + "klmnopqrst"
// BEGIN PROBE F09.P1
let eq = s1 == s2
// END PROBE F09.P1
print(eq, bufferAddress(s1) != bufferAddress(s2))
