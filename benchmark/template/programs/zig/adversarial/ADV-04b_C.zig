const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const a: i32 = -1;
    const b: u32 = 1;
    const c = a < b;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=CMP:{s}\n", .{if (c) "true" else "false"}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
