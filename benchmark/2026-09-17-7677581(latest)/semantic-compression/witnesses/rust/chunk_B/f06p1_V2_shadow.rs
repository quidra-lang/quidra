struct Vec<T> { items: [T; 3] }
impl std::ops::Index<usize> for Vec<i32> {
    type Output = i32;
    fn index(&self, i: usize) -> &i32 { &self.items[i] }
}
macro_rules! vec { ($a:expr, $b:expr, $c:expr) => { Vec { items: [$a + 10, $b + 10, $c + 10] } } }

fn probe() -> i32 {
// BEGIN PROBE F06.P1
fn mid(v: &Vec<i32>) -> i32 { v[1] }
let xs = vec![1, 2, 3];
let y = mid(&xs);
y
// END PROBE F06.P1
}

fn main() {
    println!("{}", probe());
}
