fn main() {
// BEGIN PROBE F14.P1
enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }
let s: Shape = Shape::Circle { r: 2.0 };
// END PROBE F14.P1
    if let Shape::Circle { r } = s {
        println!("{}", r);
    }
}
