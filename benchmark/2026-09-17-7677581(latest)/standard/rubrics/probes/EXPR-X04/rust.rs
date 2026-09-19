trait Speaker { fn speak(&self) -> String; }
struct Dog; struct Cat;
impl Speaker for Dog { fn speak(&self) -> String { "woof".into() } }
impl Speaker for Cat { fn speak(&self) -> String { "meow".into() } }
fn pick(n: u32) -> Box<dyn Speaker> { if n == 0 { Box::new(Dog) } else { Box::new(Cat) } }
fn main() { let s = "01"; let v: Vec<String> = s.chars().map(|c| pick(c.to_digit(10).unwrap()).speak()).collect();
    println!("X04 {} {}", v[0], v[1]); }
