const std = @import("std");

var src: i64 = 5000000000;

fn probe() i32 {
    const big: i64 = src;
// BEGIN PROBE F10.P1
const small = std.math.cast(i32, big) orelse 0;
return small;
// END PROBE F10.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
