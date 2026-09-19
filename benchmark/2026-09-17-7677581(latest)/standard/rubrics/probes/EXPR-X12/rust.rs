macro_rules! make_rec {
    ($name:ident, $a:ident, $b:ident) => {
        struct $name { $a: i32, $b: i32 }
        impl $name { const FIELD_COUNT: usize = 2; }
    };
}
make_rec!(Rec, a, b);
fn main() { let r = Rec { a: 1, b: 2 }; let _ = (r.a, r.b); println!("X12 meta {}", Rec::FIELD_COUNT); }
