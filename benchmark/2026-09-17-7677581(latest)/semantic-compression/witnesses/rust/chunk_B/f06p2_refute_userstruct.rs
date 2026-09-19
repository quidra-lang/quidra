#![allow(non_camel_case_types)]
#[derive(Clone, Copy)]
struct i32(u8);
impl std::ops::Div for i32 { type Output = i32; fn div(self, o: i32) -> i32 { i32(self.0 + o.0) } }
impl std::ops::Rem for i32 { type Output = i32; fn rem(self, o: i32) -> i32 { i32(self.0 * o.0) } }
impl std::ops::Add for i32 { type Output = i32; fn add(self, o: i32) -> i32 { i32(self.0 + o.0) } }
fn probe() -> u8 {
fn divmod2(a: i32, b: i32) -> (i32, i32) { (a / b, a % b) }
let (q, r) = divmod2(7, 3);
(q + r).0
}
fn main() { println!("{}", probe()); }
