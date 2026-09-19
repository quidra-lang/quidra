struct Upto { i: u32, n: u32 }
impl Iterator for Upto { type Item = u32;
    fn next(&mut self) -> Option<u32> { if self.i < self.n { let v = self.i; self.i += 1; Some(v) } else { None } } }
fn main() { print!("X07"); for v in (Upto { i: 0, n: 3 }) { print!(" {}", v); } println!(); }
