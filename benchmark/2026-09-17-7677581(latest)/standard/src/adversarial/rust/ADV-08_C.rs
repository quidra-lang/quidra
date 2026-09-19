use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let a: i64 = 7;
    let b: i64 = 0;
    let q: i64 = a / b;

    writeln!(out, "OBS=QUOT:{}", q).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
