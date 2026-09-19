fn probe() -> i32 {
// BEGIN PROBE F04.P1
let mut slot: i32 = 4;
let port = &mut slot;
*port = 9;
slot
// END PROBE F04.P1
}

fn main() {
    println!("{}", probe());
}
