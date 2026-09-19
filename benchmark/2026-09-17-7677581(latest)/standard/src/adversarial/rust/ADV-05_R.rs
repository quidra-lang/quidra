use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let f: f64 = line.trim().parse::<f64>().unwrap();

    let v: i32 = f as i32;

    writeln!(out, "OBS=V:{}", v).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
