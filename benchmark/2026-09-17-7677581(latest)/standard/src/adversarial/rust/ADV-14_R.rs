use std::io::BufRead;
use std::io::Write;

fn scale(n: i64) -> i64 {
    n * 3
}

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let s: String = line.trim().to_string();

    let r: i64 = scale(s);

    writeln!(out, "OBS=R:{}", r).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
