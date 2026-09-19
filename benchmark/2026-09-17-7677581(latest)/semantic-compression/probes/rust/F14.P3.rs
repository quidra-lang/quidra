mod shapes;

fn tri_area() -> f64 {
// BEGIN PROBE F14.P3
struct Tri { b: f64, h: f64 }
impl shapes::Shape for Tri {
    fn area(&self) -> f64 { 0.5 * self.b * self.h }
}
let s: &dyn shapes::Shape = &Tri { b: 3.0, h: 4.0 };
s.area()
// END PROBE F14.P3
}

fn main() {
    println!("{}", tri_area());
}
