const std = @import("std");
const Point = struct { x: i32, y: i32 };
pub fn main() void { const p = Point{ .x = 3, .y = 4 }; std.debug.print("X01 {d} {d}\n", .{ p.x, p.y }); }
