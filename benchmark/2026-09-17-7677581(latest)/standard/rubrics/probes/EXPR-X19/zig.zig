const std = @import("std");
var box: i32 = 0;
fn work() void { box = 42; }
pub fn main() !void { const t = try std.Thread.spawn(.{}, work, .{}); t.join();
    std.debug.print("X19 {d}\n", .{box}); }
