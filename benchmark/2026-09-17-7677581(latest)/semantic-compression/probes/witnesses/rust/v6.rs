#[derive(Clone, Copy, Debug)]
struct P { x: i32 }
fn read_at(xs: Vec<P>, i: i32) -> P {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(vec![P { x: 1 }, P { x: 2 }, P { x: 3 }], 2)); }
