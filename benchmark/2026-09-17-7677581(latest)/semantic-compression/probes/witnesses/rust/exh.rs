enum Shape { Circle { r: f64 }, Rect { w: f64, h: f64 } }
fn area_of(s: Shape) -> f64 {
let area: f64 = match s {
    Shape::Circle { r } => 3.141592653589793 * r * r,
};
area
}
fn main() { println!("{}", area_of(Shape::Circle { r: 2.0 })); }
