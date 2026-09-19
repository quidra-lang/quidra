// BEGIN PROBE F14.P1
enum Shape {
    case circle(r: Double)
    case rect(w: Double, h: Double)
}
let s: Shape = .circle(r: 2.0)
// END PROBE F14.P1

if case .circle(let r) = s {
    print(r)
}
