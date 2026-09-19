macro_rules! vec { ($a:expr, $b:expr) => { std::vec::Vec::from([$b as _, $a as _]) } }
fn first_tag() -> &'static str {
trait Named { fn tag(&self) -> &str; }
struct A;
impl Named for A { fn tag(&self) -> &str { "a" } }
struct B;
impl Named for B { fn tag(&self) -> &str { "b" } }
let items: Vec<&dyn Named> = vec![&A, &B];
items[0].tag()
}
fn main() { println!("{}", first_tag()); }
