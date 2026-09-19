fn main () {
    let mut state: i64 = 7;
    let mut k: i64 = 0;
    let mut total: i64 = 0;
    let mut largest: i64 = 0;
    let mut evens: i64 = 0;
    let mut joined: String = String::new();
    while k < 50 {
        state = (state * 48271) % 2147483647;
        let term: i64 = state % 1000;
        total = total + term;
        if term > largest {
            largest = term;
        }
        if term % 2 == 0 {
            evens = evens + 1;
        }
        if k < 5 {
            if k == 0 {
                joined = format!("{}", term);
            } else {
                joined = format!("{}-{}", joined, term);
            }
        }
        k = k + 1;
    }
    println!("SUM {}", total);
    println!("MAX {}", largest);
    println!("EVENS {}", evens);
    println!("JOINED {}", joined);
}
