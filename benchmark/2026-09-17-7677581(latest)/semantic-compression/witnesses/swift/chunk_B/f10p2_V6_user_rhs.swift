struct Money { var cents: Swift.Int }
func + (l: Swift.Double, r: Money) -> Money { print("user +"); return Money(cents: r.cents + Swift.Int(l)) }

let i: Int32 = 3
let d = Money(cents: 40)
// BEGIN PROBE F10.P2
let sum = Double(i) + d
// END PROBE F10.P2
print(sum.cents)
