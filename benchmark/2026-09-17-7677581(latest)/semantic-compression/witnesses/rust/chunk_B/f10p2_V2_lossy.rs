fn main() {
    let i: i64 = 9007199254740993;
    let d: f64 = 0.5;
// BEGIN PROBE F10.P2
    let sum = i as f64 + d;
// END PROBE F10.P2
    println!("{:?}", sum);
}
