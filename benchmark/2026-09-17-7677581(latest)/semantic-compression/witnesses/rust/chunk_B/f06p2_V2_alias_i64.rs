#![allow(non_camel_case_types)]
type i32 = i64;
fn probe() -> i32 {
fn divmod2(a: i32, b: i32) -> (i32, i32) { (a / b, a % b) }
let (q, r) = divmod2(7, 3);
q + r
}
fn main() { println!("{} {}", probe(), std::mem::size_of_val(&probe())); }
