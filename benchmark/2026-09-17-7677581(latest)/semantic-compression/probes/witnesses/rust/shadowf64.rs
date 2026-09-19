#![allow(non_camel_case_types)]
struct f64;
fn main() {
enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }
let s: Shape = Shape::Circle { r: 2.0 };
    if let Shape::Circle { r: _ } = s { println!("x"); }
}
