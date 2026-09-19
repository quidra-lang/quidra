fn main() {
    let mut xs = vec![3, 1, 2];
    let a: i32 = 1;
    let b: i32 = 2;
// BEGIN PROBE F09.P2
    xs.sort_by(|x, y| y.cmp(x));
    let lt = a < b;
// END PROBE F09.P2
    println!("{:?} {}", xs, lt);
}
