fn main() {
    let a: i32 = -7;
    let b: i32 = std::env::args().count() as i32 - 1;
// BEGIN PROBE F07.P2
    let q = a / b;
    let m = a % b;
    let d = a as f64 / b as f64;
// END PROBE F07.P2
    println!("{} {} {}", q, m, d);
}
