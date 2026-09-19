fn main() {
// BEGIN PROBE F19.P2
use std::sync::atomic::{AtomicI64, Ordering};

let counter = AtomicI64::new(0);
let bump = || {
    for _ in 0..1000 {
        counter.fetch_add(1, Ordering::Relaxed);
    }
};
std::thread::scope(|s| {
    s.spawn(bump);
    s.spawn(bump);
});
let total = counter.load(Ordering::Relaxed);
// END PROBE F19.P2
    println!("{}", total);
}
