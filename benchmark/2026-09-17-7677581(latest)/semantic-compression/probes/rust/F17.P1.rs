use std::fs::File;
use std::io;

// BEGIN PROBE F17.P1
fn read_all() -> io::Result<usize> {
    let file = File::open("data.txt")?;
    let text = io::read_to_string(&file)?;
    Ok(text.len())
}
// END PROBE F17.P1

fn main() {
    println!("{}", read_all().unwrap());
}
