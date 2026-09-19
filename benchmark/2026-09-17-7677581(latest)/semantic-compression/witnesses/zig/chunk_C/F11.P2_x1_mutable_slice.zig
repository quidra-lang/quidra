const std = @import("std");
const Box = struct { items: []i32 };
fn second(xs: Box) i32 {
const part = xs.items[1..4];
return part[0];
}
pub fn main() void {
    var backing = [_]i32{ 10, 20, 30, 40, 50 };
    const b: Box = .{ .items = &backing };
    const part = b.items[1..4];
    part[0] = 99;
    std.debug.print("{d} {d}\n", .{ second(b), backing[1] });
}
