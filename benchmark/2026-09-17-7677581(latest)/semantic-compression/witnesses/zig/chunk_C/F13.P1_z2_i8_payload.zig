const std = @import("shim_i8.zig");
// BEGIN PROBE F13.P1
fn parse_twice(s: []const u8) !i32 {
    return 2 * try std.fmt.parseInt(i32, s, 10);
}
// END PROBE F13.P1
pub fn main() !void {
    const o = @import("std");
    o.debug.print("{d}\n", .{try parse_twice("7")});
}
