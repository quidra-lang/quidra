mod x13 {
    fn priv_fn() -> i32 { 7 }
    pub fn pub_fn() -> i32 { priv_fn() }
}
use x13::pub_fn;
fn main() { println!("X13 {}", pub_fn()); }
