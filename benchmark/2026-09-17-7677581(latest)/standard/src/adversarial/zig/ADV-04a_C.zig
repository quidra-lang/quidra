const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const a: u32 = 0;
    const b: u32 = 1;
    const s: u32 = a - b;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=SUB:{d}\n", .{s}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
