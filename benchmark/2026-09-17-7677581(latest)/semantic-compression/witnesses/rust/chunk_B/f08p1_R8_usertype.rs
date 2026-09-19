#![allow(non_camel_case_types)]
#[derive(Clone, Copy)]
struct i32(u8);
impl i32 {
    const MAX: i32 = i32(200);
    fn checked_add(self, o: u8) -> Option<u8> { println!("user checked_add"); Some(self.0 + o) }
}

fn overflow_at_max() -> u8 {
// BEGIN PROBE F08.P1
let m = i32::MAX;
let o = m.checked_add(1).unwrap_or(0);
o
// END PROBE F08.P1
}

fn main() {
    println!("{}", overflow_at_max());
}
