use std::io::Write;

struct A {
    v: i64,
}

struct B {
    v: i64,
}

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let a = A { v: 42 };
    let r: Box<dyn std::any::Any> = Box::new(a);
    let b: Box<B> = r.downcast::<B>().unwrap();
    let v: i64 = b.v;

    writeln!(out, "OBS=V:{}", v).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
