const std = @import("std");
const Box = struct { items: []const i32 };
fn second(xs: Box) i32 {
const part = xs.items[1..4];
return part[0];
}
pub fn main() void {
    const backing = [_]i32{ 10, 20, 30, 40, 50 };
    std.debug.print("{d}\n", .{second(.{ .items = &backing })});
}
