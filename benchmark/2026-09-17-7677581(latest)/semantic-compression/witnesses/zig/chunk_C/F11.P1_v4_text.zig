const std = @import("std");
const Box = struct { items: []const []const u8 };
fn read_at(xs: Box, i: i32) []const u8 {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void {
    const backing = [_][]const u8{ "p", "q", "r" };
    std.debug.print("{s}\n", .{read_at(.{ .items = &backing }, 2)});
}
