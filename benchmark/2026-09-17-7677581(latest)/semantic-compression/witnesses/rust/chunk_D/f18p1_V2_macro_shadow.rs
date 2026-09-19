macro_rules! println {
    ($($t:tt)*) => { eprint!("stderr-instead") };
}

fn main() {
// BEGIN PROBE F18.P1
println!("x");
// END PROBE F18.P1
}
