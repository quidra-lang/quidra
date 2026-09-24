// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-04).
fn workload() -> (f64, f64, f64, f64) {
    const M: usize = 4000;
    const R: usize = 75000;

    let mut gen = Lcg::new(20264917);
    let mut a_vec = vec![0.0f64; M];
    let mut b_vec = vec![0.0f64; M];
    for i in 0..M {
        a_vec[i] = 0.5 + gen.next_unit();
    }
    for i in 0..M {
        b_vec[i] = 0.5 + gen.next_unit();
    }

    let mut s1 = 0.0f64;
    let mut s2 = 0.0f64;
    let mut s3 = 0.0f64;
    let mut s4 = 0.0f64;
    for r in 0..R {
        a_vec[r % M] = a_vec[r % M] + 1.0e-9; // anti-elimination, part of the algorithm
        for (&a, &b) in a_vec.iter().zip(b_vec.iter()) {
            s1 = s1 + a * b;
            s2 = s2 + a / (b + 2.0);
            s3 = s3 + (a * a + b * b).sqrt();
            s4 = s4 + (a - b) * (a - b);
        }
    }

    (s1, s2, s3, s4)
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

    let (s1, s2, s3, s4) = if mode == "steady" {
        let mut last = (0.0f64, 0.0f64, 0.0f64, 0.0f64);
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

    println!("MB04 s1={:.16e} s2={:.16e} s3={:.16e} s4={:.16e}", s1, s2, s3, s4);
}
