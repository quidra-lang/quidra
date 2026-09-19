fn main() { let w = i32::MAX.wrapping_add(1); let c = i32::MAX.checked_add(1);
    println!("X24 {} {}", w, if c.is_none() { "none" } else { "some" }); }
