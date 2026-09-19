func &+ (lhs: Int32, rhs: Int32) -> Int32 { print("user &+"); return 99 }

func overflowAtMax() -> Int32 {
// BEGIN PROBE F08.P1
let m = Int32.max
let o = m &+ 1
return o
// END PROBE F08.P1
}

print(overflowAtMax())
