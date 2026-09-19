struct Src;
impl Src { fn parse(&self) -> Option<i32> { panic!("user parse") } }
fn recover(s: Src) -> i32 {
let n: i32 = s.parse().unwrap_or(0);
n
}
fn main() { println!("{}", recover(Src)); }
