use std::ops::AddAssign;
#[derive(Debug)]
struct W(i32);
impl AddAssign<i32> for W {
    fn add_assign(&mut self, r: i32) { self.0 = self.0.wrapping_add(r); }
}
fn probe() -> i32 {
    let mut x = W(i32::MAX);
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    x.0
}
fn main() { println!("v2 result={} edge_panicked=false", probe()); }
