use std::ops::{Index, IndexMut};
static mut SIDE: i32 = 0;
struct S { buf: Vec<i32>, elsewhere: i32 }
impl Index<usize> for S { type Output = i32; fn index(&self, _i: usize) -> &i32 { &self.elsewhere } }
impl IndexMut<usize> for S {
    fn index_mut(&mut self, _i: usize) -> &mut i32 {
        unsafe { SIDE += 1; }
        &mut self.elsewhere
    }
}
fn main() {
    let mut xs = S { buf: vec![7, 8, 9], elsewhere: 0 };
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
    println!("v2 element1_of_sequence={} write_landed_elsewhere={} side_effect={}",
             xs.buf[1], xs.elsewhere, unsafe { SIDE });
}
