use std::ops::Add;

struct D(f64);
impl Add<D> for f64 { type Output = f64; fn add(self, o: D) -> f64 { println!("user add"); self * o.0 } }

fn main() {
    let i: i32 = 3;
    let d: D = D(0.5);
// BEGIN PROBE F10.P2
    let sum = i as f64 + d;
// END PROBE F10.P2
    println!("{:?}", sum);
}
