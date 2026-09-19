const std = @import("std");
fn probe() i64 {
    const a = 2147483647;
    const b = 2;
    const c = 0;
// BEGIN PROBE F07.P1
const r = a * b + c;
// END PROBE F07.P1
    return r;
}
pub fn main() void { std.debug.print("{d}\n", .{probe()}); }
