struct M(i32);
fn main() {
    let i: M = M(3);
    let d: f64 = 0.5;
// BEGIN PROBE F10.P2
    let sum = i as f64 + d;
// END PROBE F10.P2
    println!("{:?}", sum);
}
