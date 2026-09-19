use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let n: i64 = line.trim().parse::<i64>().unwrap();

    let mut xs: Vec<i64> = vec![0i64; n as usize];
    xs[0] = 1;
    let first: i64 = xs[0];

    writeln!(out, "OBS=ALLOC:{}|FIRST:{}", xs.len(), first).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
