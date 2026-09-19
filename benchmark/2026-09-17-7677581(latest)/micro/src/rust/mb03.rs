// MB-03 - Integer arithmetic: mixed add / xor / multiply-modulo / divide.
#![forbid(unsafe_code)]

use std::env;
use std::hint::black_box;
use std::io::Write;
use std::time::Instant;

/// The whole workload body (methodology 06 section 4, MB-03).
fn workload() -> (i64, i64, i64, i64) {
    const N: i64 = 120000000;

    let mut x: i64 = 20263917; // the seed itself is the initial generator state
    let mut s_add: i64 = 0;
    let mut s_xor: i64 = 0;
    let mut s_mul: i64 = 1;
    let mut s_div: i64 = 0;

    for _ in 0..N {
        x = (48271 * x) % 2147483647;
        s_add = (s_add + x) % 2147483647;
        s_xor ^= x;
        s_mul = (s_mul * 33 + (x % 97)) % 1000003;
        s_div += x / 1000;
    }

    (s_add, s_xor, s_mul, s_div)
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

    let (s_add, s_xor, s_mul, s_div) = if mode == "steady" {
        let mut last = (0i64, 0i64, 0i64, 0i64);
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

    println!("MB03 {} {} {} {}", s_add, s_xor, s_mul, s_div);
}
