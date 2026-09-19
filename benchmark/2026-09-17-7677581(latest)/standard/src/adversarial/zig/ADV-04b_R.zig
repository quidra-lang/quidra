const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const a: i32 = try std.fmt.parseInt(i32, try r.interface.takeSentinel('\n'), 10);
    const b: u32 = try std.fmt.parseInt(u32, try r.interface.takeSentinel('\n'), 10);

    const c = a < b;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=CMP:{s}\n", .{if (c) "true" else "false"}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
