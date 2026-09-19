use std::io::BufRead;
use std::io::Write;

fn f(n: i64) -> i64 {
    1 + f(n + 1)
}

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let n: i64 = line.trim().parse::<i64>().unwrap();

    let r: i64 = f(n);

    writeln!(out, "OBS=R:{}", r).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
