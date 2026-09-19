const std = @import("std");

// BEGIN PROBE F20.P2
export fn add2(a: c_int, b: c_int) c_int {
    return a + b;
}
// END PROBE F20.P2

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{add2(2, 3)});
    try out.interface.flush();
}
