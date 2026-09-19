const std = @import("std");
const Box = struct { items: [*]const i32 };
fn read_at(xs: Box, i: i32) i32 {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void {
    const backing = [_]i32{ 1, 2, 3 };
    std.debug.print("{d} {d}\n", .{ read_at(.{ .items = &backing }, 2), read_at(.{ .items = &backing }, 7) });
}
