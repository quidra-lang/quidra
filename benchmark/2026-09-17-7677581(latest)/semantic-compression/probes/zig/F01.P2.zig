const std = @import("std");

// BEGIN PROBE F01.P2
var counter: i64 = 0;
const LIMIT: i64 = 100;
// END PROBE F01.P2

pub fn main() !void {
    counter += LIMIT;
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{counter});
    try out.interface.flush();
}
