const std = @import("std");
pub fn main() void {
    std.debug.print("X17 {d} {d} {d} {d} {d}\n", .{
        @as(i32, @intFromFloat(@sqrt(@as(f64, 4.0)))),
        @as(i32, @intFromFloat(std.math.exp(@as(f64, 0.0)))),
        @as(i32, @intFromFloat(std.math.log(f64, std.math.e, @as(f64, 1.0)))),
        @as(i32, @intFromFloat(std.math.sin(@as(f64, 0.0)))),
        @as(i32, @intFromFloat(std.math.cos(@as(f64, 0.0)))) });
}
