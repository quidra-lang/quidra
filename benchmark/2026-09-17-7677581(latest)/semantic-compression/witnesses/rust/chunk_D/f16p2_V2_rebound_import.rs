struct UserMap;

impl UserMap {
    fn from(_entries: [(&'static str, i32); 1]) -> UserMap {
        print!("user-map ");
        UserMap
    }
    fn values(&self) -> std::vec::IntoIter<i32> {
        vec![1].into_iter()
    }
    fn get(&self, _k: &str) -> Option<&'static i32> {
        None
    }
}

use UserMap as HashMap;

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
