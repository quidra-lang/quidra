const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F18.P2
const util = @import("util.zig");

const result = util.pubAdd(2, 3);
// END PROBE F18.P2
    return result;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
