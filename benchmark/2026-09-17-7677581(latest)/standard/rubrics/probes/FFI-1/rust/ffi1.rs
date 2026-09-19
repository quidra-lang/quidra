extern "C" { fn cos(x: f64) -> f64; }
fn main() { println!("cos(1.0)={:.10}", unsafe { cos(1.0) }); }
