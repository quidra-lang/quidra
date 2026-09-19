#![allow(non_snake_case)]
fn Ok(v: i32) -> Result<i32, std::num::ParseIntError> { std::result::Result::Ok(v + 1000) }

// BEGIN
fn parse_twice(s: &str) -> Result<i32, std::num::ParseIntError> {
    Ok(s.parse::<i32>()? * 2)
}
// END
fn main() { println!("{:?}", parse_twice("21")); }
