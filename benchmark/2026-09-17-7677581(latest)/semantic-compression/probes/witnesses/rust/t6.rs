#![allow(non_camel_case_types)]
trait Ord { }
impl Ord for i32 { }
impl std::ops::Shr<i32> for Wrap { type Output = bool; fn shr(self, _: i32) -> bool { true } }
struct Wrap;

fn greater() -> i32 {
fn max_of<T: Ord>(x: T, y: T) -> T { if x > y { x } else { y } }
let m: i32 = max_of(3, 5);
m
}
fn main() { println!("{}", greater()); }
