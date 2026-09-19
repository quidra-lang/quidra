use std::collections::HashMap;

fn probe() -> i32 {
// BEGIN PROBE F16.P2
let mp = HashMap::from([("a", 1)]);
let total: i32 = mp.values().sum();
let miss = *mp.get("b").unwrap_or(&0);
total + miss
// END PROBE F16.P2
}

fn main() {
    println!("{}", probe());
}
