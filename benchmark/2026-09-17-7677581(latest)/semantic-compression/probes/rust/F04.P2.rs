fn probe() -> i32 {
    let xs: Vec<i32> = vec![4, 5, 6];
// BEGIN PROBE F04.P2
let window = &xs;
window[0]
// END PROBE F04.P2
}

fn main() {
    println!("{}", probe());
}
