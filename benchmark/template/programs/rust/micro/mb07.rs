// MB-07 - Sorting: bottom-up (iterative) merge sort, ascending, stable,
// ping-pong buffers. The pinned algorithm only; no standard-library sort.
#![forbid(unsafe_code)]

use std::env;
use std::hint::black_box;
use std::io::Write;
use std::mem;
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
}

/// Sorts `a` in place, using `buf` as the scratch half of the ping-pong pair.
fn msort(a: &mut [i64], buf: &mut [i64]) {
    let n = a.len();
    let mut src = a;
    let mut dst = buf;
    let mut flipped = false;

    let mut width = 1;
    while width < n {
        let mut lo = 0;
        while lo < n {
            let mid = (lo + width).min(n);
            let hi = (lo + 2 * width).min(n);
            let (mut i, mut j, mut k) = (lo, mid, lo);
            while i < mid && j < hi {
                if src[i] <= src[j] {
                    dst[k] = src[i];
                    i += 1;
                } else {
                    dst[k] = src[j];
                    j += 1;
                }
                k += 1;
            }
            while i < mid {
                dst[k] = src[i];
                i += 1;
                k += 1;
            }
            while j < hi {
                dst[k] = src[j];
                j += 1;
                k += 1;
            }
            lo += 2 * width;
        }
        mem::swap(&mut src, &mut dst);
        flipped = !flipped;
        width *= 2;
    }

    if flipped {
        // An odd number of passes left the sorted data in the scratch buffer,
        // so `dst` is now the caller's array: copy the result back into it.
        dst.copy_from_slice(src);
    }
}

/// The whole workload body, generation included (methodology 06 section 4, MB-07).
fn workload() -> (i64, i64, i64) {
    const N: usize = 2000000;
    const R: usize = 4;

    let mut gen = Lcg::new(20267917);
    let mut src = vec![0i64; N];
    for i in 0..N {
        src[i] = gen.next_int();
    }
    let mut buf = vec![0i64; N];

    let mut total: i64 = 0;
    let mut ssum: i64 = 0;
    let mut inv: i64 = 0;

    for r in 0..R {
        src[r] = src[r] + 1; // anti-elimination

        let mut a = src.clone(); // the per-round copy is part of the workload
        msort(&mut a, &mut buf);

        let mut chk: i64 = 0;
        for i in 0..N {
            chk = (chk * 31 + (a[i] % 1000003)) % 1000003;
        }
        total = (total * 7 + chk) % 1000003;

        ssum = 0;
        for i in 0..N {
            ssum += a[i];
        }
        for i in 1..N {
            if a[i - 1] > a[i] {
                inv += 1;
            }
        }
    }

    (total, ssum, inv)
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

    let (total, ssum, inv) = if mode == "steady" {
        let mut last = (0i64, 0i64, 0i64);
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

    println!("MB07 {} {} {}", total, ssum, inv);
}
