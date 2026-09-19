#![allow(non_camel_case_types)]
type i32 = u32;
fn probe() -> i32 {
// BEGIN PROBE F06.P1
fn mid(v: &Vec<i32>) -> i32 { v[1] }
let xs = vec![1, 2, 3];
let y = mid(&xs);
y
// END PROBE F06.P1
}

fn main() {
    let v = probe();
    println!("{} {}", v, v.wrapping_sub(3));
}
