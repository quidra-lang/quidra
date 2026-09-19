fn f(v: &Vec<i32>) {
    v.push(4);
}

fn probe() -> i32 {
let x = vec![1, 2, 3];
f(&x);
x[0]
}

fn main() { println!("{}", probe()); }
