use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let s: i32 = -1;
    let u: u32 = 1;
    let v: bool = s < u;

    writeln!(out, "OBS=CMP:{}", v).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
