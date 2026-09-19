#[no_mangle]
pub extern "C" fn add2(a: i32, b: i32) -> i32 {
    a + b
}

fn main() {
    let x = std::hint::black_box(i32::MAX);
    println!("{}", add2(x, 1));
}
