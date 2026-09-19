const std = @import("std");

fn greater() i32 {
// BEGIN PROBE F15.P2
const max_of = struct {
    fn max_of(comptime T: type, a: T, b: T) T {
        return if (a > b) a else b;
    }
}.max_of;
const m = max_of(i32, 3, 5);
return m;
// END PROBE F15.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{greater()});
    try out.interface.flush();
}
