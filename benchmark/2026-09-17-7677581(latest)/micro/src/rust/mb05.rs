// MB-05 - Vector inner product: straight-line dot product, repeated R times.
// Storage: Vec<f64>, Rust's ordinary contiguous array type.
#![forbid(unsafe_code)]

use std::env;
use std::hint::black_box;
use std::io::Write;
use std::time::Instant;

/// Lehmer / MINSTD generator, frozen for every language in the suite.
struct Lcg {
    state: i64,
}

impl Lcg {
    fn new(seed: i64) -> Self {
        Lcg { state: seed }
    }

    fn next_int(&mut self) -> i64 {
        self.state = (48271 * self.state) % 2147483647;
        self.state
    }

    fn next_unit(&mut self) -> f64 {
        self.next_int() as f64 / 2147483647.0
    }
}

/// The whole workload body, generation included (methodology 06 section 4, MB-05).
fn workload() -> f64 {
    const N: usize = 2000000;
    const R: usize = 400;

    let mut gen = Lcg::new(20265917);
    let mut x = vec![0.0f64; N];
    let mut y = vec![0.0f64; N];
    for i in 0..N {
        x[i] = 0.5 + gen.next_unit();
    }
    for i in 0..N {
        y[i] = 0.5 + gen.next_unit();
    }

    let mut total = 0.0f64;
    for r in 0..R {
        x[r] = x[r] + 1.0e-9; // anti-elimination; R <= N so no wrap
        let mut d = 0.0f64;
        for i in 0..N {
            d = d + x[i] * y[i];
        }
        total = total + d;
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
        let mut last = 0.0f64;
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

    println!("MB05 total={:.16e}", total);
}
