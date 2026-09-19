func / (lhs: Int32, rhs: Int32) -> Int32 { return 77 }

func probe() -> Int32 {
// BEGIN PROBE F06.P2
func divmod2(_ a: Int32, _ b: Int32) -> (Int32, Int32) { (a / b, a % b) }
let (q, r) = divmod2(7, 3)
return q + r
// END PROBE F06.P2
}

print(probe())
