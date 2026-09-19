fn recover(s: &str) -> i32 {
// BEGIN PROBE F13.P2
let n: i32 = s.parse().unwrap_or(0);
n
// END PROBE F13.P2
}

fn main() {
    println!("{}", recover("21"));
}
