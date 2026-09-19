use std::hint::black_box;
fn probe() -> i32 {
    let mut x: i32 = 41;
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    x
}
fn probe_edge() -> i32 {
    let mut x: i32 = black_box(i32::MAX);
    x += 1;
    x
}
fn main() {
    println!("v1 result={}", probe());
    let r = std::panic::catch_unwind(probe_edge);
    println!("v1 edge_panicked={}", r.is_err());
}
