const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const s: ?[]const u8 = null;
    const n = s.len;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=LEN:{d}\n", .{n}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
