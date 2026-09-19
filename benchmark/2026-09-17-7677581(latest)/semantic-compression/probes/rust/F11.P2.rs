fn second(xs: Vec<i32>) -> i32 {
// BEGIN PROBE F11.P2
let part = &xs[1..4];
part[0]
// END PROBE F11.P2
}

fn main() {
    println!("{}", second(vec![10, 20, 30, 40, 50]));
}
