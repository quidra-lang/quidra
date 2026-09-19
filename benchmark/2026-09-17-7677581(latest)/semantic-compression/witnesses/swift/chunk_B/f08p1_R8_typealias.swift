typealias Int32 = UInt32

func overflowAtMax() -> Int32 {
// BEGIN PROBE F08.P1
let m = Int32.max
let o = m &+ 1
return o
// END PROBE F08.P1
}

print(overflowAtMax())
