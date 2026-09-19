fn probe(cond: bool) -> i32 {
// BEGIN PROBE F02.P1
let v: i32;
if cond { v = 5; } else { v = 9; }
v
// END PROBE F02.P1
}

fn main() {
    println!("{}", probe(true));
}
