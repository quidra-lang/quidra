// DBG-1
const ITERATIONS: i64 = 300000000;
const MODULUS: i64 = 1000000007;

struct Item {
    count: i64,
    name: String,
}

fn accumulate(items: &Vec<Item>, iterations: i64) -> i64 {
    let mut total: i64 = 0;
    for _i in 0..iterations {
        for item in items.iter() {
            total = (total * 31 + item.count + item.name.len() as i64) % MODULUS;
        }
    }
    total
}

fn main() {
    let mut items: Vec<Item> = Vec::new();
    items.push(Item { count: 7, name: String::from("alpha") });
    items.push(Item { count: 11, name: String::from("bravo") });
    items.push(Item { count: 13, name: String::from("charlie") });
    let checksum = accumulate(&items, ITERATIONS);
    println!("checksum={}", checksum);
}
