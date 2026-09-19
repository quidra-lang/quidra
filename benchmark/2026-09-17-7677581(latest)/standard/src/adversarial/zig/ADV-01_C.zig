const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const a: i64 = 9223372036854775807;
    const b: i32 = @intCast(a);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=V:{d}\n", .{b}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
