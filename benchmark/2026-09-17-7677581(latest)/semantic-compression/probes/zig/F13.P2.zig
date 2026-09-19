const std = @import("std");

fn recover(s: []const u8) i32 {
// BEGIN PROBE F13.P2
const n: i32 = std.fmt.parseInt(i32, s, 10) catch 0;
return n;
// END PROBE F13.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{recover("21")});
    try out.interface.flush();
}
