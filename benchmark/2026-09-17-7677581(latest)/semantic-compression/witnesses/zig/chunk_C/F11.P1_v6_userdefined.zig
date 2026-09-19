const std = @import("std");
const P = struct { x: i32 };
const Box = struct { items: []const P };
fn read_at(xs: Box, i: i32) P {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void {
    const backing = [_]P{ .{ .x = 1 }, .{ .x = 2 }, .{ .x = 3 } };
    std.debug.print("{any}\n", .{read_at(.{ .items = &backing }, 2)});
}
