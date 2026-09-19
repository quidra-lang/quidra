const std = @import("std");

var src: i32 = 6;

fn probe() i32 {
    const a: i32 = src;
    const b: i32 = 7;
    const c: i32 = 5;
// BEGIN PROBE F07.P1
const r = a * b + c;
// END PROBE F07.P1
    return r;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
