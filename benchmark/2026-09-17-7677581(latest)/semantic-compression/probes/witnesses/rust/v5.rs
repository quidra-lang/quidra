fn read_at(xs: Vec<&'static i32>, i: i32) -> &'static i32 {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(vec![&1, &2, &3], 2)); }
