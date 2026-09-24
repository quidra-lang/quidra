use std::io::BufRead;
use std::io::Write;

fn pick(b: bool) -> i64 {
    if b {
        return 1;
    }
}

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let b: bool = line.trim() == "1";

    let r: i64 = pick(b) * 2;

    writeln!(out, "OBS=R:{}", r).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
