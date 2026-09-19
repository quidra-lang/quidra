fn inner() -> Result<i32, String> { Err("boom".to_string()) }
fn outer() -> Result<i32, String> { let v = inner()?; Ok(v) }
fn main() { match outer() { Ok(_) => {}, Err(e) => println!("X08 caught {}", e) } }
