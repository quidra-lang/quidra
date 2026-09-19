const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F04.P1
var slot: i32 = 4;
const port = &slot;
port.* = 9;
return slot;
// END PROBE F04.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
