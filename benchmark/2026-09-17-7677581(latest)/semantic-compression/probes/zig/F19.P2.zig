fn probe() !i64 {
// BEGIN PROBE F19.P2
const std = @import("std");
const S = struct {
    fn bump(c: *std.atomic.Value(i64)) void {
        for (0..1000) |_| _ = c.fetchAdd(1, .monotonic);
    }
};
var counter: std.atomic.Value(i64) = .init(0);
const t1 = try std.Thread.spawn(.{}, S.bump, .{&counter});
const t2 = try std.Thread.spawn(.{}, S.bump, .{&counter});
t1.join();
t2.join();
const total = counter.load(.monotonic);
// END PROBE F19.P2
    return total;
}

pub fn main() !void {
    const s = @import("std");
    var threaded: s.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = s.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe()});
    try out.interface.flush();
}
