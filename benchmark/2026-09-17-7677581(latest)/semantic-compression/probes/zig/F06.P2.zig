const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F06.P2
const q, const r = divmod2(7, 3);
return q + r;
}

fn divmod2(a: i32, b: i32) struct { i32, i32 } {
    return .{ @divTrunc(a, b), @rem(a, b) };
}
// END PROBE F06.P2

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
