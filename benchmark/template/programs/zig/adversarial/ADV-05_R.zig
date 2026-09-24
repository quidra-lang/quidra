const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const f: f64 = try std.fmt.parseFloat(f64, try r.interface.takeSentinel('\n'));

    const v: i32 = @intFromFloat(f);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=V:{d}\n", .{v}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
