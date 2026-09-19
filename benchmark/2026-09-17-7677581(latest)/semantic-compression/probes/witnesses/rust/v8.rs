struct Total;
impl std::ops::Index<usize> for Total {
    type Output = i32;
    fn index(&self, _i: usize) -> &i32 { &-1 }
}
fn read_at(xs: Total, i: i32) -> i32 {
let e = xs[i as usize];
e
}
fn main() { println!("{:?}", read_at(Total, 999)); }
