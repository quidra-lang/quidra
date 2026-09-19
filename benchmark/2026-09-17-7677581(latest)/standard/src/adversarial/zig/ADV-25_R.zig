const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var rd = std.Io.File.stdin().readerStreaming(io, &ib);
    const text: []const u8 = try rd.interface.takeSentinel('\n');

    const n: i64 = try std.fmt.parseInt(i64, text, 10);
    const r: i64 = n * 2;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=R:{d}\n", .{r}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
