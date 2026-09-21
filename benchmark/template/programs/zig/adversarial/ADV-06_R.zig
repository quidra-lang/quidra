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

    const m: f64 = a / b;
    const xs = [3]f64{ 3.0, m, 1.0 };
    var best: f64 = xs[0];
    for (xs[1..]) |x| {
        if (x > best) best = x;
    }
    const selfeq = m == m;

    var ob: [128]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=MAX:{d:.6}|SELFEQ:{s}\n", .{ best, if (selfeq) "true" else "false" }));
    try out.writeStreamingAll(io, "ADV-END\n");
}
