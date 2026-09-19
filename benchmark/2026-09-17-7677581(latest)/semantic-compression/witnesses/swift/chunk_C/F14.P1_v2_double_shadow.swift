struct Double: ExpressibleByFloatLiteral, CustomStringConvertible {
    var raw: Swift.Double
    init(floatLiteral value: Swift.Double) { raw = value + 100 }
    var description: Swift.String { "\(raw)" }
}
enum Shape {
    case circle(r: Double)
    case rect(w: Double, h: Double)
}
let s: Shape = .circle(r: 2.0)

if case .circle(let r) = s {
    print(r)
}
