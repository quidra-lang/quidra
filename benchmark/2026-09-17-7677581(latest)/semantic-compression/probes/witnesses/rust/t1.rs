#![allow(non_upper_case_globals)]
const None: Option<i32> = Some(7);

fn fallback() -> i32 {
let o: Option<i32> = None;
let n: i32 = o.unwrap_or(0);
n
}
fn main() { println!("{}", fallback()); }
