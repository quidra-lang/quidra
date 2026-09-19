fn probe() -> i32 {
// BEGIN PROBE F18.P2
mod util {
    pub fn pub_add(a: i32, b: i32) -> i32 {
        a + b + secret()
    }

    fn secret() -> i32 {
        1
    }
}

let result = util::pub_add(2, 3);
// END PROBE F18.P2
result
}

fn main() {
    println!("{}", probe());
}
