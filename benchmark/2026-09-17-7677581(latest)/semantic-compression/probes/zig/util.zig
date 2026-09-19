// BEGIN PROBE F18.P2
pub fn pubAdd(a: i32, b: i32) i32 {
    return a + b + secret();
}

fn secret() i32 {
    return 1;
}
// END PROBE F18.P2
