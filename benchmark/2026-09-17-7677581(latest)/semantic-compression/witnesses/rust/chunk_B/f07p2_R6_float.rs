fn main() {
    let a: f64 = -7.0;
    let b: f64 = 0.0;
// BEGIN PROBE F07.P2
    let q = a / b;
    let m = a % b;
    let d = a as f64 / b as f64;
// END PROBE F07.P2
    println!("{} {} {}", q, m, d);
}
