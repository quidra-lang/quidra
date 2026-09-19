use std::num::Wrapping;

fn main() {
    let a: Wrapping<i32> = Wrapping(2147483647);
    let b: Wrapping<i32> = Wrapping(2);
    let c: Wrapping<i32> = Wrapping(0);
// BEGIN PROBE F07.P1
    let r = a * b + c;
// END PROBE F07.P1
    println!("{}", r);
}
