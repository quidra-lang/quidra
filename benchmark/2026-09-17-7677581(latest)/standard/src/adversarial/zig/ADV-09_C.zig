const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const arr = [5]i64{ 10, 20, 30, 40, 50 };
    const xs: []const i64 = &arr;

    const z: usize = 0;
    const i: usize = z - 1;
    const e: i64 = xs[i];

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=ELEM:{d}\n", .{e}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
