use std::sync::atomic::{AtomicI32, Ordering};

static RELEASED: AtomicI32 = AtomicI32::new(0);

trait Drop {
    fn drop(&mut self);
}

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
