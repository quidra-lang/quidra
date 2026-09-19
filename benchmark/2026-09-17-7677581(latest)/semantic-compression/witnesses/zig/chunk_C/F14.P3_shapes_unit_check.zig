const std = @import("std");
const shapes = @import("shapes.zig");
pub fn main() void {
    const c: shapes.Circle = .{ .r = 2.0 };
    const r: shapes.Rect = .{ .w = 3.0, .h = 4.0 };
    const a: shapes.Shape = .{ .ptr = &c, .areaFn = shapes.Circle.area };
    const b: shapes.Shape = .{ .ptr = &r, .areaFn = shapes.Rect.area };
    std.debug.print("{d} {d}\n", .{ a.area(), b.area() });
}
