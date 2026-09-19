#[derive(PartialEq)]
struct Money(i32);
impl PartialOrd for Money {
    fn partial_cmp(&self, _o: &Money) -> Option<std::cmp::Ordering> { println!("user partial_cmp"); None }
}

fn main() {
    let mut xs = vec![3, 1, 2];
    let a = Money(1);
    let b = Money(2);
// BEGIN PROBE F09.P2
    xs.sort_by(|x, y| y.cmp(x));
    let lt = a < b;
// END PROBE F09.P2
    println!("{:?} {}", xs, lt);
}
