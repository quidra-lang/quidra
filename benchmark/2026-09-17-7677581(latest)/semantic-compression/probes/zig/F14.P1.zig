const std = @import("std");

fn build() f64 {
// BEGIN PROBE F14.P1
const Shape = union(enum) { circle: struct { r: f64 }, rect: struct { w: f64, h: f64 } };
const s: Shape = .{ .circle = .{ .r = 2.0 } };
// END PROBE F14.P1
    return s.circle.r;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{build()});
    try out.interface.flush();
}
