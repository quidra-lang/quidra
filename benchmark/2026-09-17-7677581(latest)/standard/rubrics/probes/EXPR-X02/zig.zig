const std = @import("std");
const Shape = union(enum) { circle: f64, rect: struct { w: f64, h: f64 } };
fn name(s: Shape) []const u8 { return switch (s) { .circle => "circle", .rect => "rect" }; }
pub fn main() void {
    std.debug.print("X02 {s} {s}\n", .{ name(Shape{ .circle = 1.0 }), name(Shape{ .rect = .{ .w = 2.0, .h = 3.0 } }) });
}
