fn probe() -> i32 {
// BEGIN PROBE F16.P1
let xs = vec![1, 2, 3];
let total: i32 = xs.iter().sum();
total
// END PROBE F16.P1
}

fn main() {
    println!("{}", probe());
}
