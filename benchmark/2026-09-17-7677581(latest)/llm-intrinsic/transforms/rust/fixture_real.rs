fn main() {
    let mut state: i64 = 7;
    let mut sum: i64 = 0;
    let mut max: i64 = 0;
    let mut evens: i64 = 0;
    let mut joined: String = String::new();
    let mut i: i64 = 0;
    while i < 50 {
        state = (state * 48271) % 2147483647;
        let term: i64 = state % 1000;
        sum = sum + term;
        if term > max {
            max = term;
        }
        if term % 2 == 0 {
            evens = evens + 1;
        }
        if i < 5 {
            if i == 0 {
                joined = format!("{}", term);
            } else {
                joined = format!("{}-{}", joined, term);
            }
        }
        i = i + 1;
    }
    println!("SUM {}", sum);
    println!("MAX {}", max);
    println!("EVENS {}", evens);
    println!("JOINED {}", joined);
}
