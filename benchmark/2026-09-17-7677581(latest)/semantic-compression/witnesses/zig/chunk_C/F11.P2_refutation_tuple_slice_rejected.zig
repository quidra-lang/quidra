const std = @import("std");
const Box = struct { items: struct { i32, i32, i32, i32, i32 } };
fn second(xs: Box) i32 {
const part = xs.items[1..4];
return part[0];
}
pub fn main() void {
    std.debug.print("{d}\n", .{second(.{ .items = .{ 10, 20, 30, 40, 50 } })});
}
