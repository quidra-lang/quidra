enum Shape: ~Copyable {
    case circle(r: Double)
    case rect(w: Double, h: Double)
}
func areaOf(_ s: borrowing Shape) -> Double {
let area: Double = switch s {
case let .circle(r): 3.141592653589793 * r * r
case let .rect(w, h): w * h
}
return area
}
print(areaOf(.circle(r: 2.0)))
