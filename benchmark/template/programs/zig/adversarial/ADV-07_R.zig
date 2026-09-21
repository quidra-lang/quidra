const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const a: f64 = try std.fmt.parseFloat(f64, try r.interface.takeSentinel('\n'));
    const b: f64 = try std.fmt.parseFloat(f64, try r.interface.takeSentinel('\n'));

    const h: f64 = a / b;
    const mean: f64 = (1.0 + h + 3.0) / 3.0;
    const diff: f64 = h - h;

    var ob: [128]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=MEAN:{d:.6}|DIFF:{d:.6}\n", .{ mean, diff }));
    try out.writeStreamingAll(io, "ADV-END\n");
}
