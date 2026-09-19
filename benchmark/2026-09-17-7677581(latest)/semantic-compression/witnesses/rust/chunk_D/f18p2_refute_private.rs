fn probe() -> i32 {
mod util {
    pub fn pub_add(a: i32, b: i32) -> i32 {
        a + b + secret()
    }

    fn secret() -> i32 {
        1
    }
}

let result = util::pub_add(2, 3) + util::secret();
result
}

fn main() {
    println!("{}", probe());
}
