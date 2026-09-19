fn combine() -> String {
// BEGIN PROBE F15.P1
fn head<T>(mut xs: Vec<T>) -> T { xs.remove(0) }
let a = head(vec![4, 5, 6]);
let b = head(vec!["p", "q"]);
format!("{}{}", a, b)
// END PROBE F15.P1
}

fn main() {
    println!("{}", combine());
}
