extern "c" fn cos(x: f64) f64;
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;

pub fn main() void {
    _ = printf("cos(1.0)=%.10f\n", cos(1.0));
}
