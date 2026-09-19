// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
const ITERS: i64 = 200000;

fn main() {
    let mut counter: i64 = 0;
    std::thread::scope(|s| {
        s.spawn(|| { for _ in 0..ITERS { counter = counter + 1; } });
        s.spawn(|| { for _ in 0..ITERS { counter = counter + 1; } });
    });
    println!("counter={} expected={}", counter, 2 * ITERS);
}
