fn probe() -> i32 {
// BEGIN PROBE F01.P1
let n = 7;
n
// END PROBE F01.P1
}
fn main() { let v = probe(); println!("val={} size={} signed={}", v, std::mem::size_of_val(&v), v.is_negative() || true); }
