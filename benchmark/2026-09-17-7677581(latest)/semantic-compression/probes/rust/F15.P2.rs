fn greater() -> i32 {
// BEGIN PROBE F15.P2
fn max_of<T: Ord>(x: T, y: T) -> T { if x > y { x } else { y } }
let m: i32 = max_of(3, 5);
m
// END PROBE F15.P2
}

fn main() {
    println!("{}", greater());
}
