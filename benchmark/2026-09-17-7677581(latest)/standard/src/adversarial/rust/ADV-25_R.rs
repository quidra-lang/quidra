use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let n: i64 = line.trim().parse::<i64>().unwrap();

    let r: i64 = n * 2;

    writeln!(out, "OBS=R:{}", r).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
