struct W { var v: Int32 }
func += (lhs: inout W, rhs: Int32) { }
func probe(_ start: Int32) -> Int32 {
    var x = W(v: start)
x += 1
    return x.v
}
print("v5 result=\(probe(41))")
