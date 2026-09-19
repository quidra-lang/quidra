final class Cell { var v: Int32 = 0 }
struct W { let c: Cell }
func += (lhs: inout W, rhs: Int32) { lhs.c.v += rhs }
func probe() -> String {
    let shared = Cell()
    shared.v = 41
    var x = W(c: shared)
x += 1
    return "ownStorageUnchanged=\(x.c === shared) referredState=\(shared.v)"
}
print("v4 " + probe())
