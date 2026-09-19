const std = @import("std");
var gi: i32 = 3;
var gd: f32 = 0.5;
fn probe() f64 {
    const i: i32 = gi;
    const d: f32 = gd;
// BEGIN PROBE F10.P2
const sum = @as(f64, @floatFromInt(i)) + d;
// END PROBE F10.P2
    return sum;
}
pub fn main() void { std.debug.print("{d} {s}\n", .{ probe(), @typeName(f64) }); }
