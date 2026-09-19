fn first_tag() -> &'static str {
// BEGIN PROBE F15.P3
trait Named { fn tag(&self) -> &str; }
struct A;
impl Named for A { fn tag(&self) -> &str { "a" } }
struct B;
impl Named for B { fn tag(&self) -> &str { "b" } }
let items: Vec<&dyn Named> = vec![&A, &B];
items[0].tag()
// END PROBE F15.P3
}

fn main() {
    println!("{}", first_tag());
}
