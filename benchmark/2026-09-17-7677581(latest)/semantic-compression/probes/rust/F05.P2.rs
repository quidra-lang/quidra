fn probe() -> i32 {
// BEGIN PROBE F05.P2
fn put(out: &mut i32) { *out = 12; }
let mut cell = 3;
put(&mut cell);
cell
// END PROBE F05.P2
}

fn main() {
    println!("{}", probe());
}
