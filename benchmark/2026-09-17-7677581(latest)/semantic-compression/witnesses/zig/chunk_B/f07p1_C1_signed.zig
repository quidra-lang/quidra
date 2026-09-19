const std = @import("std");
var src: i32 = 2147483647;
fn probe() i32 {
    const a: i32 = src;
    const b: i32 = 1;
    const c: i32 = 0;
// BEGIN PROBE F07.P1
const r = a * b + c;
// END PROBE F07.P1
    return r;
}
pub fn main() void { std.debug.print("{d}\n", .{probe()}); }
