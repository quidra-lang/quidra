use std::cell::RefCell;
use std::rc::Rc;

struct Node {
    id: i32,
}

fn probe() -> bool {
// BEGIN PROBE F04.P3
let first = Rc::new(RefCell::new(Node { id: 5 }));
let second = first.clone();
let same = Rc::ptr_eq(&first, &second);
same
// END PROBE F04.P3
}

fn main() {
    println!("{}", probe());
}
