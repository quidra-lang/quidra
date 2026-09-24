use std::io::Write;

fn main() {
    let mut out = std::io::stdout();
    writeln!(out, "ADV-START").unwrap();
    out.flush().unwrap();

    let mut xs: Vec<i64> = vec![1, 2, 3, 4, 5];
    let mut iters: i64 = 0;
    for x in &xs {
        iters += 1;
        if *x == 2 {
            xs.push(99);
        }
    }

    writeln!(out, "OBS=ITERS:{}|LEN:{}", iters, xs.len()).unwrap();
    out.flush().unwrap();
    writeln!(out, "ADV-END").unwrap();
    out.flush().unwrap();
}
