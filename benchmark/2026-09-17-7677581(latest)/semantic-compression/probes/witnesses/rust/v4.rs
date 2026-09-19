fn read_at(xs: Vec<&'static str>, i: i32) -> &'static str {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(vec!["p", "q", "r"], 2)); }
