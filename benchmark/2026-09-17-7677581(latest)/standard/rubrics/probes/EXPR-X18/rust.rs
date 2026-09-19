use std::fs; use std::path::Path;
fn main() { fs::write("x18.txt", "hello").unwrap();
    let d = fs::read_to_string("x18.txt").unwrap();
    println!("X18 {} {}", d, Path::new("x18.txt").exists()); }
