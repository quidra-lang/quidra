const std = @import("std");
const Shape = union(enum) { circle: struct { r: f64 }, rect: struct { w: f64, h: f64 } };
const s: Shape = .{ .tri = .{ .b = 1.0 } };
pub fn main() void { _ = s; }
