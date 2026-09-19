const std = @import("std");

fn probe() u8 {
// BEGIN PROBE F02.P2
var buf: [16]u8 = undefined;
buf[0] = 1;
return buf[0];
// END PROBE F02.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
