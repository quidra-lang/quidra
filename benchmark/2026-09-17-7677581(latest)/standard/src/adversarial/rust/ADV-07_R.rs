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

    let h: f64 = a / b;
    let mean: f64 = (1.0 + h + 3.0) / 3.0;
    let diff: f64 = h - h;

    writeln!(out, "OBS=MEAN:{:.6}|DIFF:{:.6}", mean, diff).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
