const std = @import("std");

fn probe() i32 {
    var x: i32 = 41;
// BEGIN PROBE F03.P1
x += 1;
// END PROBE F03.P1
    return x;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
