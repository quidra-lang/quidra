fn main() { let mut buf = [0i32; 4];
    { let view: &mut [i32] = &mut buf[1..3]; view[0] = 99; view[1] = 99; }
    println!("X23 {} {}", buf[1], buf[2]); }
