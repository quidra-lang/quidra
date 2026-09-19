const std = @import("std");
var gi: i64 = 9007199254740993;
var gd: f64 = 0.0;
fn probe() f64 {
    const i: i64 = gi;
    const d: f64 = gd;
// BEGIN PROBE F10.P2
const sum = @as(f64, @floatFromInt(i)) + d;
// END PROBE F10.P2
    return sum;
}
pub fn main() void { std.debug.print("{d}\n", .{probe()}); }
