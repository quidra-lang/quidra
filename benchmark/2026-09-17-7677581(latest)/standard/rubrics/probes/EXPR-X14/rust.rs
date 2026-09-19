use std::collections::{HashMap, HashSet};
fn main() { let v = vec![1, 2, 3];
    let mut m = HashMap::new(); m.insert("a", 1); m.insert("b", 2);
    let mut s: HashSet<i32> = HashSet::new(); s.insert(1); s.insert(2); s.insert(3); s.insert(1);
    println!("X14 {} {} {}", v.len(), m["b"], s.len()); }
