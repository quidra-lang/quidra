#[derive(Debug)]
struct Text(String);
impl PartialEq for Text {
    fn eq(&self, _o: &Text) -> bool { println!("user eq"); panic!("user equality panicked") }
}

fn main() {
    let s1 = Text(String::from("abcd"));
    let s2 = Text(String::from("abcd"));
// BEGIN PROBE F09.P1
    let eq = s1 == s2;
// END PROBE F09.P1
    println!("{}", eq);
}
