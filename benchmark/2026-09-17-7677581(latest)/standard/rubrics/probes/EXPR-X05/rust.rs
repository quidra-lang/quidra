fn make_adder(n: i32) -> impl Fn(i32) -> i32 { move |x| x + n }
fn main() { let f = make_adder(10); println!("X05 {}", f(5)); }
