const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    var ib: [64]u8 = undefined;
    var rd = std.Io.File.stdin().readerStreaming(io, &ib);
    var arr: [7]i64 = undefined;
    for (&arr) |*slot| {
        slot.* = try std.fmt.parseInt(i64, try rd.interface.takeSentinel('\n'), 10);
    }
    const xs: []i64 = arr[0..];
    const target: i64 = try std.fmt.parseInt(i64, try rd.interface.takeSentinel('\n'), 10);

    var lo: i64 = 0;
    var hi: i64 = 6;
    var result: i64 = -1;
    while (lo <= hi) {
        const mid = @divTrunc(lo + hi, 2);
        const v = xs[@intCast(mid)];
        if (v == target) {
            result = mid;
            break;
        }
        if (v < target) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=IDX:{d}\n", .{result}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
