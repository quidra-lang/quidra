// MB-08 - Strings: text construction plus five explicit character-level passes.
// Passes 1-4 run on the pinned mutable byte buffer (section 4.12(b): Vec<u8>);
// pass 5 runs on a native String built fresh each round, read with `.chars()`.
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
}

/// Order-sensitive rolling hash over a byte sequence.
fn rhash(seq: &[u8]) -> i64 {
    let mut h: i64 = 0;
    for &c in seq {
        h = (h * 131 + c as i64) % 1000000007;
    }
    h
}

/// The whole workload body, build phase included (methodology 06 section 4, MB-08).
fn workload() -> (i64, i64, i64, i64) {
    const NW: usize = 200000;
    const R: usize = 20;
    const Q: i64 = 1000000007;

    let mut gen = Lcg::new(20268917);

    let mut words: Vec<String> = Vec::new();
    for _ in 0..NW {
        let l = 4 + (gen.next_int() % 13);
        let mut word = String::new();
        for _ in 0..l {
            word.push((b'a' + (gen.next_int() % 26) as u8) as char);
        }
        words.push(word);
    }
    let mut text = words.join(" ").into_bytes();
    let len = text.len();

    let mut acc: i64 = 0;
    let mut cnt_ab: i64 = 0;
    let mut cnt_w: i64 = 0;

    for r in 0..R {
        let p = 7 * r + 11; // anti-elimination; p <= 144 < len
        if text[p] == b' ' {
            text[p] = b'x';
        } else {
            text[p] = b'a' + (text[p] - b'a' + 1) % 26;
        }

        let h1 = rhash(&text); // pass 1

        let mut u = vec![0u8; len]; // pass 2: upper-case
        for i in 0..len {
            let c = text[i];
            u[i] = if c >= 97 && c <= 122 { c - 32 } else { c };
        }
        let h2 = rhash(&u);

        let mut v = vec![0u8; len]; // pass 3: reverse
        for i in 0..len {
            v[i] = text[len - 1 - i];
        }
        let h3 = rhash(&v);

        cnt_ab = 0; // pass 4: naive search
        for i in 0..len - 1 {
            if text[i] == b'a' && text[i + 1] == b'b' {
                cnt_ab += 1;
            }
        }

        // pass 5: word count on the language's own string type. The String is
        // constructed fresh from the working buffer inside the timed round and
        // read with Rust's ordinary in-order character API, `.chars()`
        // (section 4.12(b): Rust offers no integer character index).
        let s = String::from_utf8(text.clone()).expect("the working buffer is ASCII");
        cnt_w = 1;
        for c in s.chars() {
            if c == ' ' {
                cnt_w += 1;
            }
        }

        for value in [h1, h2, h3, cnt_ab, cnt_w] {
            acc = (acc * 31 + value) % Q;
        }
    }

    (acc, len as i64, cnt_ab, cnt_w)
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

    let (acc, len, cnt_ab, cnt_w) = if mode == "steady" {
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

    println!("MB08 {} {} {} {}", acc, len, cnt_ab, cnt_w);
}
