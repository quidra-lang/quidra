const std = @import("std");
var src: f64 = 2.5;
fn probe() f64 {
    const a: f64 = src;
    const b: f64 = 2.0;
    const c: f64 = 0.5;
// BEGIN PROBE F07.P1
const r = a * b + c;
// END PROBE F07.P1
    return r;
}
pub fn main() void { std.debug.print("{d}\n", .{probe()}); }
