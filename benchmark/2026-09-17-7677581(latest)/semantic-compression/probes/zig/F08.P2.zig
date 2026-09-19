const std = @import("std");

var src: i32 = 0;

fn probe() i32 {
    const a: i32 = 7;
    const b: i32 = src;
// BEGIN PROBE F08.P2
const q = std.math.divTrunc(i32, a, b) catch 0;
return q;
// END PROBE F08.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
