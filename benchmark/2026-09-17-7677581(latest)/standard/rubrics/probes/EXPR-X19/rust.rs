use std::thread;
fn main() { let h = thread::spawn(|| 42); println!("X19 {}", h.join().unwrap()); }
