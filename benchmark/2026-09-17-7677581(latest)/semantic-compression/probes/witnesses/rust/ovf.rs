fn mapped(o: Option<i32>) -> i32 {
let h = |x: i32| x + 1;
let p: Option<i32> = o.map(h);
p.unwrap_or(0)
}
fn main() { println!("{}", mapped(Some(i32::MAX))); }
