use std::alloc::{GlobalAlloc, Layout, System};
use std::sync::atomic::{AtomicUsize, Ordering};
static ALLOCS: AtomicUsize = AtomicUsize::new(0);
static DEALLOCS: AtomicUsize = AtomicUsize::new(0);
struct Counting;
unsafe impl GlobalAlloc for Counting {
    unsafe fn alloc(&self, l: Layout) -> *mut u8 { ALLOCS.fetch_add(1, Ordering::SeqCst); unsafe { System.alloc(l) } }
    unsafe fn dealloc(&self, p: *mut u8, l: Layout) { DEALLOCS.fetch_add(1, Ordering::SeqCst); unsafe { System.dealloc(p, l) } }
}
#[global_allocator]
static A: Counting = Counting;

fn probe() -> (i32, usize, usize) {
    let xs: Option<i32> = Some(1);
    let k: i32 = 0;
    let a0 = ALLOCS.load(Ordering::SeqCst); let d0 = DEALLOCS.load(Ordering::SeqCst);
// BEGIN PROBE F05.P3
let neg = |a: i32| k - a;
let ys: Vec<i32> = xs.into_iter().map(neg).collect();
ys[0]
// END PROBE F05.P3
    ;
    (ys[0], ALLOCS.load(Ordering::SeqCst) - a0, DEALLOCS.load(Ordering::SeqCst) - d0)
}
fn main() { let (v,a,d) = probe(); println!("f05p3_opt result={} allocs={} deallocs={}", v, a, d); }
