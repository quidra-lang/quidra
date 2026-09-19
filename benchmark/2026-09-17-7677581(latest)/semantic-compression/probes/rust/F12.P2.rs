fn mapped(o: Option<i32>) -> i32 {
// BEGIN PROBE F12.P2
let h = |x: i32| x + 1;
let p: Option<i32> = o.map(h);
p.unwrap_or(0)
// END PROBE F12.P2
}

fn main() {
    println!("{}", mapped(None));
}
