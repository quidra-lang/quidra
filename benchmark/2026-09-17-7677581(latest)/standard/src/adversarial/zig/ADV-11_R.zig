const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var r = std.Io.File.stdin().readerStreaming(io, &ib);
    const n: i64 = try std.fmt.parseInt(i64, try r.interface.takeSentinel('\n'), 10);

    const xs: []i64 = try std.heap.page_allocator.alloc(i64, @intCast(n));
    xs[0] = 1;
    const first: i64 = xs[0];

    var ob: [128]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=ALLOC:{d}|FIRST:{d}\n", .{ xs.len, first }));
    try out.writeStreamingAll(io, "ADV-END\n");
}
