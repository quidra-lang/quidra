fn main() {
// BEGIN PROBE F19.P1
let first = std::thread::spawn(|| 20);
let second = std::thread::spawn(|| 22);
let sum = first.join().unwrap() + second.join().unwrap();
// END PROBE F19.P1
    println!("{}", sum);
}
