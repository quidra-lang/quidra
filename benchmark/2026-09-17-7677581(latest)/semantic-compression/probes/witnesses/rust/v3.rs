fn read_at(xs: Vec<f64>, i: i32) -> f64 {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(vec![1.5, 2.5, 3.5], 2)); }
