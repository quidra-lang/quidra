func triArea() -> Double {
// BEGIN PROBE F14.P3
struct Tri: Shape {
    var b: Double
    var h: Double
    func area() -> Double { 0.5 * b * h }
}
let s: Shape = Tri(b: 3.0, h: 4.0)
return s.area()
// END PROBE F14.P3
}

@main
struct Main {
    static func main() {
        print(triArea())
    }
}
