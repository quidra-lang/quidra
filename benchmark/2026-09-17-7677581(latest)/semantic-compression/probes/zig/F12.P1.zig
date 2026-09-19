const std = @import("std");

fn fallback() i32 {
// BEGIN PROBE F12.P1
const o: ?i32 = null;
const n: i32 = o orelse 0;
return n;
// END PROBE F12.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{fallback()});
    try out.interface.flush();
}
