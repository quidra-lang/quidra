fn main() {
// BEGIN PROBE F20.P1
extern "C" {
    fn abs(input: i32) -> i32;
}

let magnitude = unsafe { abs(-3) };
// END PROBE F20.P1
    println!("{}", magnitude);
}
