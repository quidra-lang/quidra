use std::ops::AddAssign;
struct W(i32);
impl AddAssign<i32> for W {
    fn add_assign(&mut self, _r: i32) {}
}
fn probe() -> i32 {
    let mut x = W(41);
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    x.0
}
fn main() { println!("v4 result={} (unchanged)", probe()); }
