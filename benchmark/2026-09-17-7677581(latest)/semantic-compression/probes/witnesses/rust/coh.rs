impl std::str::FromStr for i32 { type Err = std::num::ParseIntError; fn from_str(_s: &str) -> Result<i32, Self::Err> { Ok(7) } }
fn main() { println!("{:?}", "21".parse::<i32>()); }
