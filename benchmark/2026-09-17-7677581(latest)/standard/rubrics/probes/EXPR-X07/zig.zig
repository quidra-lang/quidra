const std = @import("std");
const Upto = struct { i: u32 = 0, n: u32,
    fn next(self: *Upto) ?u32 { if (self.i < self.n) { const v = self.i; self.i += 1; return v; } return null; } };
pub fn main() void { var it = Upto{ .n = 3 };
    std.debug.print("X07", .{});
    while (it.next()) |v| std.debug.print(" {d}", .{v});
    std.debug.print("\n", .{}); }
