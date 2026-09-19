trait HasVal { fn val(&self) -> i32; }
struct C;
impl HasVal for C { fn val(&self) -> i32 { 7 } }
fn get<T: HasVal>(t: &T) -> i32 { t.val() }
fn main() { println!("X16 {}", get(&C)); }
