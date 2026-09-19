const std = @import("std");
const P = struct { x: i32 };
fn greater() P {
const max_of = struct {
    fn max_of(comptime T: type, a: T, b: T) T {
        return if (a > b) a else b;
    }
}.max_of;
const m = max_of(P, .{ .x = 1 }, .{ .x = 2 });
return m;
}
pub fn main() void { _ = greater(); }
