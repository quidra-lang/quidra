const std = @import("std");
pub fn main() void { const pair = .{ @as(i32, 1), @as(i32, 2) }; const a, const b = pair;
    std.debug.print("X06 {d} {d}\n", .{ a, b }); }
