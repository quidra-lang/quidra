fn probe() -> i32 {
    let mut x: i32 = 41;
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    x
}

fn main() {
    println!("{}", probe());
}
