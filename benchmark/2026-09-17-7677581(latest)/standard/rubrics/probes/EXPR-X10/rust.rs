use std::ops::Add;
#[derive(Clone, Copy)] struct V { x: i32, y: i32 }
impl Add for V { type Output = V; fn add(self, o: V) -> V { V { x: self.x + o.x, y: self.y + o.y } } }
fn main() { let v = V { x: 1, y: 2 } + V { x: 3, y: 4 }; println!("X10 {} {}", v.x, v.y); }
