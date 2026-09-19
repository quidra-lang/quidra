struct Box<T> { v: T }
impl<T: Clone> Box<T> { fn get(&self) -> T { self.v.clone() } }
fn main() { println!("X03 {} {}", Box { v: 5i32 }.get(), Box { v: "hi".to_string() }.get()); }
