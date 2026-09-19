struct W { var v: Int32 }
func += (lhs: inout W, rhs: Int32) { lhs.v = lhs.v &+ rhs }
func probe(_ start: Int32) -> Int32 {
    var x = W(v: start)
x += 1
    return x.v
}
print("v3 result=\(probe(41)) edge=\(probe(Int32.max))")
