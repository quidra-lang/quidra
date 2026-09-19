fn probe() -> i32 {
    let xs: Vec<i32> = vec![1, 2, 3];
    let k: i32 = 0;
// BEGIN PROBE F05.P3
let neg = |a: i32| k - a;
let ys: Vec<i32> = xs.into_iter().map(neg).collect();
ys[0]
// END PROBE F05.P3
}

fn main() {
    println!("{}", probe());
}
