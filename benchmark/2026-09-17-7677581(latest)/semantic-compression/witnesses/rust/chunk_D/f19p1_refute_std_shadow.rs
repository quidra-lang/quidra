mod std {
    pub mod thread {
        pub struct H(pub i32);
        impl H {
            pub fn join(self) -> Result<i32, ()> { Ok(self.0) }
        }
        pub fn spawn<F: FnOnce() -> i32>(f: F) -> H {
            print!("fake ");
            H(f())
        }
    }
}

fn main() {
// BEGIN PROBE F19.P1
let first = std::thread::spawn(|| 20);
let second = std::thread::spawn(|| 22);
let sum = first.join().unwrap() + second.join().unwrap();
// END PROBE F19.P1
    ::std::println!("{}", sum);
}
