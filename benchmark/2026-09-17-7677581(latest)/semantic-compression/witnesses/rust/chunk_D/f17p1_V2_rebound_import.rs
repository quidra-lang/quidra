mod fake {
    pub struct File;
    impl File {
        pub fn open(_p: &str) -> super::io::Result<File> {
            print!("no-fd ");
            Ok(File)
        }
    }
}

mod io {
    pub type Result<T> = std::result::Result<T, ()>;
    pub fn read_to_string(_f: &super::fake::File) -> Result<String> {
        Ok(String::from("xyz"))
    }
}

use fake::File;

// BEGIN PROBE F17.P1
fn read_all() -> io::Result<usize> {
    let file = File::open("data.txt")?;
    let text = io::read_to_string(&file)?;
    Ok(text.len())
}
// END PROBE F17.P1

fn main() {
    println!("{}", read_all().unwrap());
}
