const std = @import("std");

var src: i32 = -7;

fn probe() struct { i32, i32, f64 } {
    const a: i32 = src;
    const b: i32 = 3;
// BEGIN PROBE F07.P2
const q = @divTrunc(a, b);
const m = @rem(a, b);
const d = @as(f64, @floatFromInt(a)) / @as(f64, @floatFromInt(b));
// END PROBE F07.P2
    return .{ q, m, d };
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const t = probe();
    try out.interface.print("{d} {d} {d}\n", .{ t[0], t[1], t[2] });
    try out.interface.flush();
}
