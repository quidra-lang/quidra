final class Box { var v = 3 }
func readAt(_ xs: [Box], _ i: Int) -> Box {
let e = xs[i]
return e
}
let b = Box()
let r = readAt([Box(), Box(), b], 2)
r.v = 9
print(b.v)
