struct Double: ExpressibleByFloatLiteral, CustomStringConvertible {
    var raw: Swift.Double
    init(floatLiteral value: Swift.Double) { raw = value }
    init(_ v: Swift.Double) { raw = v }
    static func * (a: Double, b: Double) -> Double { Double(a.raw * b.raw + 1000) }
    var description: Swift.String { "\(raw)" }
}

func triArea() -> Double {
struct Tri: Shape {
    var b: Double
    var h: Double
    func area() -> Double { 0.5 * b * h }
}
let s: Shape = Tri(b: 3.0, h: 4.0)
return s.area()
}

@main
struct Main {
    static func main() {
        print(triArea())
    }
}
