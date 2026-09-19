enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }
fn area_of(s: Shape) -> f64 {
let area: f64 = match s {
    Shape::Circle { r } => 3.141592653589793 * r * r,
    Shape::Rect { w, h } => w * h,
};
area
}
fn main() { let s = Shape::Rect { w: 2.0, h: 3.0 }; println!("{} {}", area_of(s), area_of(s)); }
