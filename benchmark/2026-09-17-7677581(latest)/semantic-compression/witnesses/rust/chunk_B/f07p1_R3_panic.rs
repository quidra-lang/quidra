fn main() {
    let a: i32 = 2147483646 + std::env::args().count() as i32;
    let b: i32 = 2;
    let c: i32 = 0;
// BEGIN PROBE F07.P1
    let r = a * b + c;
// END PROBE F07.P1
    println!("{}", r);
}
