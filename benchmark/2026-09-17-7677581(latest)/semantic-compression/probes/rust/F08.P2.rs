fn guarded(a: i32, b: i32) -> i32 {
// BEGIN PROBE F08.P2
let q = a.checked_div(b).unwrap_or(0);
q
// END PROBE F08.P2
}

fn main() {
    println!("{}", guarded(7, 0));
}
