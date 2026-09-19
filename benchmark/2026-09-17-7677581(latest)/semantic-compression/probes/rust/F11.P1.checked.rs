fn read_at(xs: Vec<i32>, i: i32) -> i32 {
// BEGIN PROBE F11.P1.checked
let e = *xs.get(i as usize).unwrap_or(&0);
e
// END PROBE F11.P1.checked
}

fn main() {
    println!("{}", read_at(vec![1, 2, 3], 2));
}
