#[derive(Clone, Copy)]
enum Option<T> { Some(T) }
impl Option<i32> {
    fn map<F: Fn(i32) -> i32>(self, f: F) -> Option<i32> { let Option::Some(v) = self; Option::Some(f(v) * 10) }
    fn unwrap_or(self, _d: i32) -> i32 { let Option::Some(v) = self; v }
}
fn mapped(o: Option<i32>) -> i32 {
let h = |x: i32| x + 1;
let p: Option<i32> = o.map(h);
p.unwrap_or(0)
}
fn main() { println!("{}", mapped(Option::Some(4))); }
