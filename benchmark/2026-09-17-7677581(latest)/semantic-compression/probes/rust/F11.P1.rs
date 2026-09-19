fn read_at(xs: Vec<i32>, i: i32) -> i32 {
// BEGIN PROBE F11.P1
let e = xs[i as usize];
e
// END PROBE F11.P1
}

fn main() {
    println!("{}", read_at(vec![1, 2, 3], 2));
}
