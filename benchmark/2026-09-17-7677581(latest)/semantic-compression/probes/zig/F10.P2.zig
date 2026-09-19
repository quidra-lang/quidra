const std = @import("std");

var src: i32 = 3;

fn probe() f64 {
    const i: i32 = src;
    const d: f64 = 0.5;
// BEGIN PROBE F10.P2
const sum = @as(f64, @floatFromInt(i)) + d;
// END PROBE F10.P2
    return sum;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
