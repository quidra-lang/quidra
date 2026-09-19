use std::cell::Cell;
use std::ops::Index;
struct S { hits: Cell<i32>, val: i32 }
impl Index<usize> for S {
    type Output = i32;
    fn index(&self, _i: usize) -> &i32 { self.hits.set(self.hits.get() + 1); &self.val }
}
fn probe() -> i32 {
    let xs = S { hits: Cell::new(0), val: 4 };
// BEGIN PROBE F04.P2
let window = &xs;
window[0]
// END PROBE F04.P2
}
fn main() {
    let xs = S { hits: Cell::new(0), val: 4 };
    let window = &xs;
    let v = window[0];
    println!("v2 value={} mutation_through_shared_view={}", v, xs.hits.get());
    println!("v2 probe={}", probe());
}
