use std::convert::TryFrom;

fn narrow(big: u8) -> i32 {
// BEGIN PROBE F10.P1
let small = i32::try_from(big).unwrap_or(0);
small
// END PROBE F10.P1
}

fn main() {
    println!("{}", narrow(200));
}
