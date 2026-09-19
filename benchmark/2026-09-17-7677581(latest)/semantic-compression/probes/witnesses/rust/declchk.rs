fn max_of<T: Ord>(x: T, y: T) -> T { if x.abs() > y.abs() { x } else { y } }
fn main() { println!("{}", max_of(3, 5)); }
