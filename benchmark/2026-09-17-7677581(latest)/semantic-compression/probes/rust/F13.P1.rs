// BEGIN PROBE F13.P1
fn parse_twice(s: &str) -> Result<i32, std::num::ParseIntError> {
    Ok(s.parse::<i32>()? * 2)
}
// END PROBE F13.P1

fn main() {
    println!("{}", parse_twice("21").unwrap());
}
