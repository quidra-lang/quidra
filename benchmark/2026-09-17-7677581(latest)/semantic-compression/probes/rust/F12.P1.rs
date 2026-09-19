fn fallback() -> i32 {
// BEGIN PROBE F12.P1
let o: Option<i32> = None;
let n: i32 = o.unwrap_or(0);
n
// END PROBE F12.P1
}

fn main() {
    println!("{}", fallback());
}
