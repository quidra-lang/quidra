use std::convert::TryFrom;

struct Big(i64);
impl TryFrom<Big> for i32 {
    type Error = ();
    fn try_from(v: Big) -> Result<i32, ()> { println!("user try_from"); panic!("user conversion panicked: {}", v.0) }
}

fn narrow(big: Big) -> i32 {
// BEGIN PROBE F10.P1
let small = i32::try_from(big).unwrap_or(0);
small
// END PROBE F10.P1
}

fn main() {
    println!("{}", narrow(Big(2147483648)));
}
