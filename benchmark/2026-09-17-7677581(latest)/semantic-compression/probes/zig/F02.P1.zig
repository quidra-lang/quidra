const std = @import("std");

fn probe(cond: bool) i32 {
// BEGIN PROBE F02.P1
var v: i32 = undefined;
if (cond) v = 5 else v = 9;
return v;
// END PROBE F02.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe(true)});
    try out.interface.flush();
}
