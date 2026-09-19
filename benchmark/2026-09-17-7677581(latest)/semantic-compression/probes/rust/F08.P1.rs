fn overflow_at_max() -> i32 {
// BEGIN PROBE F08.P1
let m = i32::MAX;
let o = m.checked_add(1).unwrap_or(0);
o
// END PROBE F08.P1
}

fn main() {
    println!("{}", overflow_at_max());
}
