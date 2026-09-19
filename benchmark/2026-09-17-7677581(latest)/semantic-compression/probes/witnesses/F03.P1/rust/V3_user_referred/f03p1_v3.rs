use std::cell::Cell;
use std::ops::AddAssign;
use std::rc::Rc;
struct W(Rc<Cell<i32>>);
impl AddAssign<i32> for W {
    fn add_assign(&mut self, r: i32) { self.0.set(self.0.get() + r); }
}
fn main() {
    let shared = Rc::new(Cell::new(41));
    let mut x = W(shared.clone());
    let before = Rc::as_ptr(&x.0);
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    println!("v3 own_storage_unchanged={} referred_state={}", before == Rc::as_ptr(&x.0), shared.get());
}
