use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let xs: Vec<i64> = vec![10, 20, 30, 40, 50];

    let mut line = String::new();
    std::io::stdin().lock().read_line(&mut line).unwrap();
    let z: usize = line.trim().parse::<usize>().unwrap();

    let i: usize = z - 1;
    let e: i64 = xs[i];

    writeln!(out, "OBS=ELEM:{}", e).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
