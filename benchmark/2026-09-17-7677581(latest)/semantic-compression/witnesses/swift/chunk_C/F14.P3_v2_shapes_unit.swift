protocol Shape {
    func area() -> Double
}
struct Circle: Shape {
    var r: Double
    func area() -> Double { 3.141592653589793 * r * r }
}
struct Rect: Shape {
    var w: Double
    var h: Double
    func area() -> Double { w * h }
}
