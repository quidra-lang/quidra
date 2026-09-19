const fn fact(n: u32) -> u32 { if n <= 1 { 1 } else { n * fact(n - 1) } }
const V: u32 = fact(5);
const _: () = assert!(V == 120);
fn main() { let arr = [0u8; fact(5) as usize]; println!("X11 {}", arr.len()); }
