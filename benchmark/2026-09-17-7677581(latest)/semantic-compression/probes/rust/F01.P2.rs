// BEGIN PROBE F01.P2
static mut COUNTER: i64 = 0;
const LIMIT: i64 = 100;
// END PROBE F01.P2

fn main() {
    let total = unsafe {
        COUNTER += LIMIT;
        COUNTER
    };
    println!("{}", total);
}
