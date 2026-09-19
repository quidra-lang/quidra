#![allow(non_camel_case_types)]
type i32 = u32;
fn probe() -> i32 {
// BEGIN PROBE F06.P2
fn divmod2(a: i32, b: i32) -> (i32, i32) { (a / b, a % b) }
let (q, r) = divmod2(7, 3);
q + r
// END PROBE F06.P2
}

fn main() {
    let v = probe();
    println!("{} {}", v, v.wrapping_sub(4));
}
