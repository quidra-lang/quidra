use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};

static ALLOCS: AtomicUsize = AtomicUsize::new(0);
static COUNTING: AtomicUsize = AtomicUsize::new(0);

struct Counter;
unsafe impl GlobalAlloc for Counter {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 {
        if COUNTING.load(Ordering::Relaxed) == 1 { ALLOCS.fetch_add(1, Ordering::Relaxed); }
        unsafe { System.alloc(l) }
    }
    unsafe fn alloc_zeroed(&self, l: Layout) -> *mut u8 {
        if COUNTING.load(Ordering::Relaxed) == 1 { ALLOCS.fetch_add(1, Ordering::Relaxed); }
        unsafe { System.alloc_zeroed(l) }
    }
    unsafe fn realloc(&self, p: *mut u8, l: Layout, n: usize) -> *mut u8 {
        if COUNTING.load(Ordering::Relaxed) == 1 { ALLOCS.fetch_add(1, Ordering::Relaxed); }
        unsafe { System.realloc(p, l, n) }
    }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { unsafe { System.dealloc(p, l) } }
}

#[global_allocator]
static A: Counter = Counter;

fn main() {
    let mut xs = vec![3, 1, 2];
    let a: i32 = 1;
    let b: i32 = 2;
    COUNTING.store(1, Ordering::Relaxed);
// BEGIN PROBE F09.P2
    xs.sort_by(|x, y| y.cmp(x));
    let lt = a < b;
// END PROBE F09.P2
    COUNTING.store(0, Ordering::Relaxed);
    println!("{:?} {} allocs_len3={}", xs, lt, ALLOCS.load(Ordering::Relaxed));

    for n in [30usize, 400, 100_000] {
        let mut big: Vec<i32> = (0..n as i32).map(|k| (k * 7919) % 1001).collect();
        ALLOCS.store(0, Ordering::Relaxed);
        COUNTING.store(1, Ordering::Relaxed);
        big.sort_by(|x, y| y.cmp(x));
        COUNTING.store(0, Ordering::Relaxed);
        println!("len{} allocs={} first={}", n, ALLOCS.load(Ordering::Relaxed), big[0]);
    }
}
