// MB-01 - Fibonacci: naive double recursion, n = 30..37 inclusive.
#![forbid(unsafe_code)]

use std::env;
use std::hint::black_box;
use std::io::Write;
use std::time::Instant;

fn fib(n: u32) -> i64 {
    if n < 2 {
        return i64::from(n);
    }
    fib(n - 1) + fib(n - 2)
}

/// The whole workload body (methodology 06 section 4, MB-01).
fn workload() -> i64 {
    let mut total: i64 = 0;
    for n in 30..=37 {
        total += fib(n);
    }
    total
}

fn main() {
    // Methodology 06 section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K.
    let args: Vec<String> = env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("once");
    let k: usize = args
        .get(2)
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(7)
        .max(1);

    let total = if mode == "steady" {
        let mut last: i64 = 0;
        for i in 0..k {
            let start = Instant::now();
            last = black_box(workload());
            let elapsed_ns = start.elapsed().as_nanos();
            println!("ITER {} {}", i, elapsed_ns);
            std::io::stdout().flush().expect("stdout flush");
        }
        last
    } else {
        workload()
    };

    println!("MB01 {}", total);
}
