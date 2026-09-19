fn second(xs: Vec<i32>) -> i32 {
let part = &xs[1..4];
part[0]
}
fn main() { println!("{}", second(vec![10, 20, 30])); }
