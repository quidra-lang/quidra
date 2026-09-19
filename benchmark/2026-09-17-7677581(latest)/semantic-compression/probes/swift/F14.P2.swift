enum Shape {
    case circle(r: Double)
    case rect(w: Double, h: Double)
}

func areaOf(_ s: Shape) -> Double {
// BEGIN PROBE F14.P2
let area: Double = switch s {
case let .circle(r): 3.141592653589793 * r * r
case let .rect(w, h): w * h
}
return area
// END PROBE F14.P2
}

print(areaOf(.circle(r: 2.0)))
