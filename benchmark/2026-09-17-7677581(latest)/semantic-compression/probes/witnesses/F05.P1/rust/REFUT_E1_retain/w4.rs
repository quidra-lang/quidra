static mut KEEP: Option<&'static Vec<i32>> = None;

fn f(v: &'static Vec<i32>) {
    unsafe { KEEP = Some(v); }
}

fn probe() -> i32 {
let x = vec![1, 2, 3];
f(&x);
x[0]
}

fn main() { println!("{}", probe()); }
