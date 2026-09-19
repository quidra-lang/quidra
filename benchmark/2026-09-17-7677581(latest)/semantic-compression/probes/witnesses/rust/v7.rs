fn read_at(xs: Vec<*const i32>, i: i32) -> *const i32 {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(vec![std::ptr::null(), std::ptr::null(), std::ptr::null()], 2)); }
