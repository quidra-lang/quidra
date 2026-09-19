use std::ops::{Add, Mul};

#[derive(Clone, Copy)]
struct M(i32);
impl Mul for M { type Output = M; fn mul(self, o: M) -> M { println!("user mul"); M(self.0 + o.0) } }
impl Add for M { type Output = M; fn add(self, o: M) -> M { M(self.0 * o.0) } }

fn main() {
    let a: M = M(7);
    let b: M = M(6);
    let c: M = M(5);
// BEGIN PROBE F07.P1
    let r = a * b + c;
// END PROBE F07.P1
    println!("{}", r.0);
}
