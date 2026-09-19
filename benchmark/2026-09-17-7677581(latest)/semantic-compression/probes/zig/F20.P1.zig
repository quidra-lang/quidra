const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F20.P1
const c = struct {
    extern "c" fn abs(n: c_int) c_int;
};
const magnitude: i32 = c.abs(-3);
// END PROBE F20.P1
    return magnitude;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
