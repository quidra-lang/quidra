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
    let a: f64 = l1.trim().parse::<f64>().unwrap();
    let b: f64 = l2.trim().parse::<f64>().unwrap();

    let m: f64 = a / b;
    let xs: [f64; 3] = [3.0, m, 1.0];
    let mut best: f64 = xs[0];
    for i in 1..3 {
        let x: f64 = xs[i];
        if x > best {
            best = x;
        }
    }
    let selfeq: bool = m == m;

    writeln!(out, "OBS=MAX:{:.6}|SELFEQ:{}", best, selfeq).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
