fn main() {
    let a = String::from("abcd");
    let b = String::from("abcd");
    let s1: *const u8 = a.as_ptr();
    let s2: *const u8 = b.as_ptr();
// BEGIN PROBE F09.P1
    let eq = s1 == s2;
// END PROBE F09.P1
    println!("{} {}", eq, unsafe { *s1 == *s2 });
}
