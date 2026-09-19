const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F01.P1
const n: i32 = 7;
return n;
// END PROBE F01.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
