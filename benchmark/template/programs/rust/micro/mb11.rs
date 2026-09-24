// MB-11 - Collections: the standard HashMap, HashSet and Vec under
// insert / update / lookup / delete / iterate. Default construction only:
// no capacity hint and the default RandomState hasher.
#![forbid(unsafe_code)]

use std::collections::{HashMap, HashSet};
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
}

/// The whole workload body, generation included (methodology 06 section 4, MB-11).
fn workload() -> (i64, i64, i64, i64, i64, i64, i64, i64) {
    const N: usize = 1000000;
    const R: usize = 3;
    const KM: i64 = 500009;
    const SM: i64 = 100003;
    const Q: i64 = 1000000007;

    let mut gen = Lcg::new(20271917);
    let mut keys = vec![0i64; N];
    for i in 0..N {
        keys[i] = gen.next_int();
    }

    let mut acc: i64 = 0;
    let mut size1: i64 = 0;
    let mut found: i64 = 0;
    let mut vsum: i64 = 0;
    let mut mchk: i64 = 0;
    let mut size2: i64 = 0;
    let mut size3: i64 = 0;
    let mut lsum: i64 = 0;

    for r in 0..R {
        keys[r] += 1000000; // anti-elimination; stays < 2^31

        let mut m: HashMap<i64, i64> = HashMap::new();
        for i in 0..N {
            let k = keys[i] % KM;
            *m.entry(k).or_insert(0) += 1;
        }
        size1 = m.len() as i64;

        found = 0;
        vsum = 0;
        for i in 0..N {
            let k = (keys[i] + 7) % KM;
            if let Some(&v) = m.get(&k) {
                found += 1;
                vsum += v;
            }
        }

        mchk = 0;
        for (&k, &v) in &m {
            mchk = (mchk + (k % 1000003) * v) % 1000003;
        }

        for i in (0..N).step_by(2) {
            let k = keys[i] % KM;
            m.remove(&k);
        }
        size2 = m.len() as i64;

        let mut st: HashSet<i64> = HashSet::new();
        for i in 0..N {
            st.insert(keys[i] % SM);
        }
        size3 = st.len() as i64;

        let mut lst: Vec<i64> = Vec::new();
        for i in 0..N {
            lst.push(keys[i] % 1000);
        }
        lsum = 0;
        for &v in &lst {
            lsum += v;
        }

        for value in [size1, found, vsum, mchk, size2, size3, lsum] {
            acc = (acc * 31 + value) % Q;
        }
    }

    (acc, size1, found, vsum, mchk, size2, size3, lsum)
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

    let (acc, size1, found, vsum, mchk, size2, size3, lsum) = if mode == "steady" {
        let mut last = (0i64, 0i64, 0i64, 0i64, 0i64, 0i64, 0i64, 0i64);
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
        "MB11 {} {} {} {} {} {} {} {}",
        acc, size1, found, vsum, mchk, size2, size3, lsum
    );
}
