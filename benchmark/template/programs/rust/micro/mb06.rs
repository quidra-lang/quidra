// MB-06 - Matrix multiplication: classical i-j-k order over flat row-major storage.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-06).
fn workload() -> (f64, f64, f64) {
    const N: usize = 512;
    const R: usize = 3;

    let mut gen = Lcg::new(20266917);
    let mut a = vec![0.0f64; N * N];
    let mut b = vec![0.0f64; N * N];
    let mut c = vec![0.0f64; N * N];
    for i in 0..N * N {
        a[i] = gen.next_unit();
    }
    for i in 0..N * N {
        b[i] = gen.next_unit();
    }

    for r in 0..R {
        a[r] = a[r] + 1.0e-9; // anti-elimination, part of the algorithm
        for i in 0..N {
            for j in 0..N {
                let mut s = 0.0f64;
                for k in 0..N {
                    s = s + a[i * N + k] * b[k * N + j];
                }
                c[i * N + j] = s;
            }
        }
    }

    let mut sum_c = 0.0f64;
    for i in 0..N * N {
        sum_c = sum_c + c[i];
    }

    (sum_c, c[0], c[N * N - 1])
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

    let (sum_c, c_first, c_last) = if mode == "steady" {
        let mut last = (0.0f64, 0.0f64, 0.0f64);
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

    println!(
        "MB06 sumC={:.16e} c_first={:.16e} c_last={:.16e}",
        sum_c, c_first, c_last
    );
}
