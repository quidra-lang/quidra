// TEST-1 probe, Rust. Exactly 3 tests using the first-party #[test] attribute:
// two assert a true condition, one asserts a false condition.
pub fn add(a: i32, b: i32) -> i32 {
    a + b
}

#[cfg(test)]
mod tests {
    use super::add;

    #[test]
    fn pass_one() {
        assert!(add(1, 1) == 2);
    }

    #[test]
    fn pass_two() {
        assert!(add(2, 3) == 5);
    }

    #[test]
    fn fail_one() {
        assert!(add(1, 1) == 3);
    }
}
