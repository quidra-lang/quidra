const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const a: i64 = 7;
    const b: i64 = 0;
    const q: i64 = @divTrunc(a, b);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=QUOT:{d}\n", .{q}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
