#[derive(Clone, Copy)]
struct M(f64);
impl std::ops::Mul<M> for f64 { type Output = M; fn mul(self, r: M) -> M { M(self * r.0) } }
impl std::ops::Mul<M> for M { type Output = f64; fn mul(self, r: M) -> f64 { panic!("user Mul: {} {}", self.0, r.0) } }
enum Shape { Circle { r: M }, Rect { w: M, h: M } }
fn area_of(s: Shape) -> f64 {
let area: f64 = match s {
    Shape::Circle { r } => 3.141592653589793 * r * r,
    Shape::Rect { w, h } => w * h,
};
area
}
fn main() { println!("{}", area_of(Shape::Rect { w: M(2.0), h: M(3.0) })); }
