const std = @import("std");

fn scale(n: i64) i64 {
    return n * 3;
}

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const text: []const u8 = try r.interface.takeSentinel('\n');

    const v = scale(text);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=R:{d}\n", .{v}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
