fn probe() -> i32 {
    let mut xs: Vec<i32> = vec![7, 8, 9];
// BEGIN PROBE F03.P2
xs[1] = 42;
// END PROBE F03.P2
    xs[1]
}

fn main() {
    println!("{}", probe());
}
