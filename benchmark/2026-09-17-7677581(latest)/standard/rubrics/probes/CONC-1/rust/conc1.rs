// CONC-1, Rust. Worker mechanism: std::thread::scope scoped threads (std).
const N: i64 = 500000000;
const CHUNKS: usize = 4;
const SPAN: i64 = N / CHUNKS as i64;

fn chunk_sum(c: usize) -> f64 {
    let mut s = 0.0f64;
    let start = c as i64 * SPAN;
    let end = start + SPAN;
    let mut i = start;
    while i < end { let x = (i as f64).sin(); s += x * x; i += 1; }
    s
}

fn main() {
    let w_count: usize = std::env::args().nth(1).unwrap_or_else(|| "1".to_string())
        .parse().unwrap();
    let mut partial = vec![0.0f64; CHUNKS];
    let results: Vec<Vec<(usize, f64)>> = std::thread::scope(|scope| {
        let mut hs = Vec::new();
        for w in 0..w_count {
            hs.push(scope.spawn(move || {
                let mut out = Vec::new();
                for c in 0..CHUNKS { if c % w_count == w { out.push((c, chunk_sum(c))); } }
                out
            }));
        }
        hs.into_iter().map(|h| h.join().unwrap()).collect()
    });
    for r in results { for (c, v) in r { partial[c] = v; } }
    let mut total = 0.0f64;
    for c in 0..CHUNKS { total = total + partial[c]; }
    println!("workers={} result={:.10}", w_count, total);
}
