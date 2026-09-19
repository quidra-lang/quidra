const std = @import("std");
const x13 = @import("zig_mod.zig");      // explicit import
pub fn main() void { std.debug.print("X13 {d}\n", .{x13.pubFn()}); }
