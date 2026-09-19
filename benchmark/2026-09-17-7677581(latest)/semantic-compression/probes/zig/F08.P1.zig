const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F08.P1
const m: i32 = std.math.maxInt(i32);
const o = std.math.add(i32, m, 1) catch 0;
return o;
// END PROBE F08.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
