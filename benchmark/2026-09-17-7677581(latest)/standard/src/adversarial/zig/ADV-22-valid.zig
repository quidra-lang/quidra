const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const v: i64 = 1;

    var outputbuffer: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&outputbuffer, "OBS=V:{d}\n", .{v}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
