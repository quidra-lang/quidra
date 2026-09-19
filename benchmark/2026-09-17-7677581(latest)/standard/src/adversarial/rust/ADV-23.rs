use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let s: String = std::fs::read_to_string("inputs/ADV-23.bin").unwrap();

    writeln!(out, "OBS=CP:{}", s.len()).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
