const std = @import("std");

// BEGIN PROBE F13.P1
fn parse_twice(s: []const u8) !i32 {
    return 2 * try std.fmt.parseInt(i32, s, 10);
}
// END PROBE F13.P1

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try parse_twice("x")});
    try out.interface.flush();
}
