fn main() {
    let base = String::from("ab");
    let s1 = base.clone() + "cd";
    let s2 = base + "cd";
// BEGIN PROBE F09.P1
    let eq = s1 == s2;
// END PROBE F09.P1
    println!("{} {}", eq, s1.as_ptr() != s2.as_ptr());
}
