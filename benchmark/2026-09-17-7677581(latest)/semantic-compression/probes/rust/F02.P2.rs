use std::mem::MaybeUninit;

fn probe() -> u8 {
// BEGIN PROBE F02.P2
let mut buf = [MaybeUninit::<u8>::uninit(); 16];
buf[0].write(1);
unsafe { buf[0].assume_init() }
// END PROBE F02.P2
}

fn main() {
    println!("{}", probe());
}
