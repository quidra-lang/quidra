fn f(v: Vec<i32>) -> i32 {
    v[0]
}

fn probe() -> i32 {
// BEGIN PROBE F05.P1
let x = vec![1, 2, 3];
f(x);
x[0]
// END PROBE F05.P1
}

fn main() {
    println!("{}", probe());
}
