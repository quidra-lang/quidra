// MB-10 - File I/O: buffered text write of three files, then buffered
// read-back with integer parsing. Ordinary standard-library buffered I/O,
// with the 65536-byte buffer section 4.12(c) freezes for every configuration.
#![forbid(unsafe_code)]

use std::env;
use std::fs::File;
use std::hint::black_box;
use std::io::{BufRead, BufReader, BufWriter, Write};
use std::time::Instant;

/// Frozen buffer size for every configuration (methodology 06 section 4.12(c)).
const BUF: usize = 65536;

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

/// The whole workload body (methodology 06 section 4, MB-10).
fn workload() -> std::io::Result<(i64, i64, i64, i64)> {
    const N: i64 = 1000000;
    const R: i64 = 3;

    let mut gen = Lcg::new(20270917); // the stream continues across rounds
    let mut sum_v: i64 = 0;
    let mut chk: i64 = 0;
    let mut nbytes: i64 = 0;
    let mut lines: i64 = 0;

    for r in 0..R {
        let name = format!("mb10_round_{}.txt", r);

        let mut writer = BufWriter::with_capacity(BUF, File::create(&name)?);
        for i in 0..N {
            let v = gen.next_int();
            let line = format!("{} {}\n", i, v);
            writer.write_all(line.as_bytes())?;
            nbytes += line.len() as i64;
        }
        writer.flush()?;
        drop(writer);

        let reader = BufReader::with_capacity(BUF, File::open(&name)?);
        let mut idx: i64 = 0;
        for line in reader.lines() {
            let line = line?;
            let (field0, field1) = line
                .split_once(' ')
                .expect("each line holds exactly one space");
            let a: i64 = field0.parse().expect("field 0 is a decimal integer");
            let v: i64 = field1.parse().expect("field 1 is a decimal integer");
            assert_eq!(a, idx, "line index read back does not match the one written");
            idx += 1;
            lines += 1;
            sum_v = (sum_v + v) % 1000000007;
            chk = (chk * 31 + (v % 1000003)) % 1000003;
        }
    }

    Ok((sum_v, chk, nbytes, lines))
}

fn main() -> std::io::Result<()> {
    // Methodology 06 section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K.
    let args: Vec<String> = env::args().collect();
    let mode = args.get(1).map(String::as_str).unwrap_or("once");
    let k: usize = args
        .get(2)
        .and_then(|s| s.parse::<usize>().ok())
        .unwrap_or(7)
        .max(1);

    let (sum_v, chk, nbytes, lines) = if mode == "steady" {
        let mut last = (0i64, 0i64, 0i64, 0i64);
        for i in 0..k {
            let start = Instant::now();
            last = black_box(workload()?);
            let elapsed_ns = start.elapsed().as_nanos();
            println!("ITER {} {}", i, elapsed_ns);
            std::io::stdout().flush().expect("stdout flush");
        }
        last
    } else {
        workload()?
    };

    println!("MB10 {} {} {} {}", sum_v, chk, nbytes, lines);
    Ok(())
}
