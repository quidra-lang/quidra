macro_rules! vec { ($($x:expr),*) => { std::vec::Vec::from([$( $x ),*][1..].to_vec()) } }
fn combine() -> String {
fn head<T>(mut xs: Vec<T>) -> T { xs.remove(0) }
let a = head(vec![4, 5, 6]);
let b = head(vec!["p", "q"]);
format!("{}{}", a, b)
}
fn main() { println!("{}", combine()); }
