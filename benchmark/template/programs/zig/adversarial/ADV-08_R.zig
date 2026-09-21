const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const a: i64 = try std.fmt.parseInt(i64, try r.interface.takeSentinel('\n'), 10);
    const b: i64 = try std.fmt.parseInt(i64, try r.interface.takeSentinel('\n'), 10);

    const q: i64 = @divTrunc(a, b);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=QUOT:{d}\n", .{q}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
