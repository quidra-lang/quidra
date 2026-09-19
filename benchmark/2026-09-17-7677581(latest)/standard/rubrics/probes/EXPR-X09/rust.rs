struct Res;
impl Drop for Res { fn drop(&mut self) { print!("X09 cleanup "); } }
fn f() -> Result<(), String> { let _r = Res; Err("boom".into()) }
fn main() { if f().is_err() { println!("caught"); } }
