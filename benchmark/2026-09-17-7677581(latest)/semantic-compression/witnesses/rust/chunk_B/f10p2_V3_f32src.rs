fn main() {
    let i: f32 = f32::NAN;
    let d: f64 = 0.5;
// BEGIN PROBE F10.P2
    let sum = i as f64 + d;
// END PROBE F10.P2
    println!("{:?}", sum);
}
