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
    let a: u32 = l1.trim().parse::<u32>().unwrap();
    let b: u32 = l2.trim().parse::<u32>().unwrap();

    let v: u32 = a - b;

    writeln!(out, "OBS=SUB:{}", v).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
