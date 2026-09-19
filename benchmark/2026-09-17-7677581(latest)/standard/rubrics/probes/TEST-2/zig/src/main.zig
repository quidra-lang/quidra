const std = @import("std");
const mod_a = @import("mod_a.zig");
const mod_b = @import("mod_b.zig");

pub fn main() void {
    std.debug.print("scale(2)={d} scaleAndOffset(2)={d}\n", .{ mod_a.scale(2), mod_b.scaleAndOffset(2) });
}
