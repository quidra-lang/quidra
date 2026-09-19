struct M {
    var v: Double
}
func * (a: Double, b: M) -> Double { fatalError("user *") }
func * (a: M, b: M) -> Double { fatalError("user *") }
enum Shape {
    case circle(r: M)
    case rect(w: M, h: M)
}
func areaOf(_ s: Shape) -> Double {
let area: Double = switch s {
case let .circle(r): 3.141592653589793 * r * r
case let .rect(w, h): w * h
}
return area
}
print(areaOf(.circle(r: M(v: 2.0))))
