func fallback() -> Int32 {
// BEGIN PROBE F12.P1
let o: Int32? = nil
let n: Int32 = o ?? 0
return n
// END PROBE F12.P1
}

print(fallback())
