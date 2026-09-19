use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let stdin = std::io::stdin();
    let mut l1 = String::new();
    stdin.lock().read_line(&mut l1).unwrap();
    let mut l2 = String::new();
    stdin.lock().read_line(&mut l2).unwrap();
    let a: i64 = l1.trim().parse::<i64>().unwrap();
    let b: i64 = l2.trim().parse::<i64>().unwrap();

    let q: i64 = a / b;

    writeln!(out, "OBS=QUOT:{}", q).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
