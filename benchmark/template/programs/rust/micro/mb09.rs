// MB-09 - Statistics: two-pass (stable) moments, Pearson correlation and a
// 64-bin histogram. No statistics library, no Welford substitution.
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

/// The whole workload body, generation included (methodology 06 section 4, MB-09).
fn workload() -> (f64, f64, f64, f64, f64, f64, f64, i64) {
    const N: usize = 2000000;
    const R: usize = 30;

    let mut gen = Lcg::new(20269917);
    let mut x = vec![0.0f64; N];
    let mut y = vec![0.0f64; N];
    for i in 0..N {
        x[i] = gen.next_unit() * 100.0;
    }
    for i in 0..N {
        y[i] = gen.next_unit() * 100.0;
    }

    let mut mean = 0.0f64;
    let mut var = 0.0f64;
    let mut sd = 0.0f64;
    let mut mn = 0.0f64;
    let mut mx = 0.0f64;
    let mut mad = 0.0f64;
    let mut pearson = 0.0f64;
    let mut hist_chk: i64 = 0;

    for r in 0..R {
        x[r] = x[r] + 1.0e-9; // anti-elimination

        let mut s = 0.0f64; // pass 1
        mn = x[0];
        mx = x[0];
        for i in 0..N {
            let v = x[i];
            s = s + v;
            if v < mn {
                mn = v;
            }
            if v > mx {
                mx = v;
            }
        }
        mean = s / N as f64;

        let mut sq = 0.0f64; // pass 2
        let mut ad = 0.0f64;
        for i in 0..N {
            let d = x[i] - mean;
            sq = sq + d * d;
            ad = ad + if d < 0.0 { -d } else { d };
        }
        var = sq / N as f64;
        sd = var.sqrt();
        mad = ad / N as f64;

        let mut sy = 0.0f64; // pass 3
        for i in 0..N {
            sy = sy + y[i];
        }
        let meany = sy / N as f64;

        let mut sxy = 0.0f64; // pass 4
        let mut sxx = 0.0f64;
        let mut syy = 0.0f64;
        for i in 0..N {
            let dx = x[i] - mean;
            let dy = y[i] - meany;
            sxy = sxy + dx * dy;
            sxx = sxx + dx * dx;
            syy = syy + dy * dy;
        }
        pearson = sxy / (sxx * syy).sqrt();

        let mut hist = [0i64; 64]; // pass 5
        for i in 0..N {
            let mut b = (x[i] * 0.64).floor() as i64;
            if b < 0 {
                b = 0;
            }
            if b > 63 {
                b = 63;
            }
            hist[b as usize] += 1;
        }
        hist_chk = 0;
        for b in 0..64 {
            hist_chk += (b as i64 + 1) * hist[b];
        }
    }

    (mean, var, sd, mn, mx, mad, pearson, hist_chk)
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

    let (mean, var, sd, mn, mx, mad, pearson, hist_chk) = if mode == "steady" {
        let mut last = (0.0f64, 0.0f64, 0.0f64, 0.0f64, 0.0f64, 0.0f64, 0.0f64, 0i64);
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
        "MB09 mean={:.16e} var={:.16e} sd={:.16e} min={:.16e} max={:.16e} mad={:.16e} pearson={:.16e} hist_chk={}",
        mean, var, sd, mn, mx, mad, pearson, hist_chk
    );
}
