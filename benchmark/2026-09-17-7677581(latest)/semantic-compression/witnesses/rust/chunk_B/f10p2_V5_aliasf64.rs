#![allow(non_camel_case_types)]
type f64 = f32;

fn main() {
    let i: i32 = 16777217;
    let d: f64 = 0.5;
// BEGIN PROBE F10.P2
    let sum = i as f64 + d;
// END PROBE F10.P2
    println!("{:?} {}", sum, std::mem::size_of_val(&sum));
}
