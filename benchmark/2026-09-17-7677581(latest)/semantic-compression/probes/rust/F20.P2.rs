// BEGIN PROBE F20.P2
#[no_mangle]
pub extern "C" fn add2(a: i32, b: i32) -> i32 {
    a + b
}
// END PROBE F20.P2

fn main() {
    let f: extern "C" fn(i32, i32) -> i32 = std::hint::black_box(add2);
    println!("{}", f(2, 3));
}
