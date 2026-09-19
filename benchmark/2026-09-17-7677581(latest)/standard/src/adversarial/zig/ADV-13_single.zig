const std = @import("std");

const A = struct { v: i64 };
const B = struct { v: i64 };

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var a = A{ .v = 42 };
    const p: *anyopaque = @ptrCast(&a);
    const b: *B = @ptrCast(@alignCast(p));
    const v: i64 = b.v;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=V:{d}\n", .{v}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
