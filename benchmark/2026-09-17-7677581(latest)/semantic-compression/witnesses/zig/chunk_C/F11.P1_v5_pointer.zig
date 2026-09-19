const std = @import("std");
const Box = struct { items: []const *const i32 };
fn read_at(xs: Box, i: i32) *const i32 {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void {
    const a: i32 = 1; const b: i32 = 2; const c: i32 = 3;
    const backing = [_]*const i32{ &a, &b, &c };
    std.debug.print("{d}\n", .{read_at(.{ .items = &backing }, 2).*});
}
