struct Zeros;
impl std::ops::Index<std::ops::Range<usize>> for Zeros {
    type Output = [i32];
    fn index(&self, _r: std::ops::Range<usize>) -> &[i32] { &[7, 7, 7] }
}
fn second(xs: Zeros) -> i32 {
let part = &xs[1..4];
part[0]
}
fn main() { println!("{}", second(Zeros)); }
