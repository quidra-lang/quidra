use std::num::Saturating;

fn main() {
    let a: Saturating<i32> = Saturating(2147483647);
    let b: Saturating<i32> = Saturating(2);
    let c: Saturating<i32> = Saturating(0);
// BEGIN PROBE F07.P1
    let r = a * b + c;
// END PROBE F07.P1
    println!("{}", r);
}
