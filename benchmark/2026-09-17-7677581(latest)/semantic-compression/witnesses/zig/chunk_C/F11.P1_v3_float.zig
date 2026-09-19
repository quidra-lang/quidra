const std = @import("std");
const Box = struct { items: []const f64 };
fn read_at(xs: Box, i: i32) f64 {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void {
    const backing = [_]f64{ 1, 2, 3 };
    std.debug.print("{any}\n", .{read_at(.{ .items = &backing }, 2)});
}
