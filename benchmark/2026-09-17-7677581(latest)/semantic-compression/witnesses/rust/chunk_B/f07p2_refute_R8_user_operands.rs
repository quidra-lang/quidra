use std::ops::{Div, Rem};

#[derive(Clone, Copy)]
struct M(i32);
impl Div for M { type Output = M; fn div(self, o: M) -> M { M(self.0 - o.0) } }
impl Rem for M { type Output = M; fn rem(self, o: M) -> M { M(self.0 + o.0) } }

fn main() {
    let a: M = M(-7);
    let b: M = M(2);
// BEGIN PROBE F07.P2
    let q = a / b;
    let m = a % b;
    let d = a as f64 / b as f64;
// END PROBE F07.P2
    println!("{} {} {}", q.0, m.0, d);
}
