func recover(_ s: String) -> Int32 {
// BEGIN PROBE F13.P2
let n = Int32(s) ?? 0
return n
// END PROBE F13.P2
}

print(recover("21"))
