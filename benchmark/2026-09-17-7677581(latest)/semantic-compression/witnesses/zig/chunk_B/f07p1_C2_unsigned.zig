const std = @import("std");
var src: u8 = 200;
fn probe() u8 {
    const a: u8 = src;
    const b: u8 = 1;
    const c: u8 = 56;
// BEGIN PROBE F07.P1
const r = a * b + c;
// END PROBE F07.P1
    return r;
}
pub fn main() void { std.debug.print("{d}\n", .{probe()}); }
