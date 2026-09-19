struct Counter;

enum Ord2 { Relaxed }

use Ord2 as Ordering;

impl Counter {
    fn fetch_add(&self, _n: i32, _o: Ordering) {
        print!("hooked ");
    }
    fn load(&self, _o: Ordering) -> i32 {
        7
    }
}

static RELEASED: Counter = Counter;

fn main() {
// BEGIN PROBE F17.P2
struct Handle {
    id: i32,
}

impl Drop for Handle {
    fn drop(&mut self) {
        RELEASED.fetch_add(1, Ordering::Relaxed);
    }
}

{
    let _h = Handle { id: 1 };
}

let result = RELEASED.load(Ordering::Relaxed);
// END PROBE F17.P2
    println!("{}", result);
}
