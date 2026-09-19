#[derive(Eq, PartialEq)]
struct Key(i32);
impl Ord for Key {
    fn cmp(&self, o: &Key) -> std::cmp::Ordering { println!("user cmp"); if self.0 == 1 { panic!("user cmp panicked") } self.0.cmp(&o.0) }
}
impl PartialOrd for Key { fn partial_cmp(&self, o: &Key) -> Option<std::cmp::Ordering> { Some(self.cmp(o)) } }

fn main() {
    let mut xs = vec![Key(3), Key(1), Key(2)];
    let a: i32 = 1;
    let b: i32 = 2;
// BEGIN PROBE F09.P2
    xs.sort_by(|x, y| y.cmp(x));
    let lt = a < b;
// END PROBE F09.P2
    println!("{} {}", xs[0].0, lt);
}
