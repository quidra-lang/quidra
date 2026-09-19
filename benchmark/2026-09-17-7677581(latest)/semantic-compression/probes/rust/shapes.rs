// BEGIN PROBE F14.P3
pub trait Shape {
    fn area(&self) -> f64;
}
pub struct Circle { pub r: f64 }
impl Shape for Circle {
    fn area(&self) -> f64 { 3.141592653589793 * self.r * self.r }
}
pub struct Rect { pub w: f64, pub h: f64 }
impl Shape for Rect {
    fn area(&self) -> f64 { self.w * self.h }
}
// END PROBE F14.P3
