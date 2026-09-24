use std::io::BufRead;
use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let stdin = std::io::stdin();
    let mut lines = stdin.lock().lines();
    let mut xs: Vec<i64> = Vec::new();
    for _ in 0..7 {
        xs.push(lines.next().unwrap().unwrap().trim().parse::<i64>().unwrap());
    }
    let target: i64 = lines.next().unwrap().unwrap().trim().parse::<i64>().unwrap();

    let mut lo: i64 = 0;
    let mut hi: i64 = 6;
    let mut result: i64 = -1;
    while lo <= hi {
        let mid: i64 = (lo + hi) / 2;
        if xs[mid as usize] == target {
            result = mid;
            break;
        }
        if xs[mid as usize] < target {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    writeln!(out, "OBS=IDX:{}", result).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
