const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const Fns = struct {
        fn pick(b: bool) i64 {
            if (b) return 1;
        }
    };

    var ib: [64]u8 = undefined;
    var rd = std.Io.File.stdin().readerStreaming(io, &ib);
    const b: bool = std.mem.eql(u8, try rd.interface.takeSentinel('\n'), "1");

    const r: i64 = Fns.pick(b) * 2;

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=R:{d}\n", .{r}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
