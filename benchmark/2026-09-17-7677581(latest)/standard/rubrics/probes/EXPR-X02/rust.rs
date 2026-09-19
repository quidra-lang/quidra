enum Shape { Circle(f64), Rect(f64, f64) }
fn name(s: &Shape) -> &'static str { match s { Shape::Circle(_) => "circle", Shape::Rect(_, _) => "rect" } }
fn main() { println!("X02 {} {}", name(&Shape::Circle(1.0)), name(&Shape::Rect(2.0, 3.0))); }
