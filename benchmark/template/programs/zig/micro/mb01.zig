// MB-01 - Fibonacci: naive double recursion, n = 30..37 inclusive.
const std = @import("std");

fn fib(n: u32) u64 {
    if (n < 2) return n;
    return fib(n - 1) + fib(n - 2);
}

/// The whole workload body (section 5.2: this is what `steady` re-runs).
fn workload() u64 {
    var total: u64 = 0;
    var n: u32 = 30;
    while (n <= 37) : (n += 1) {
        total += fib(n);
    }
    return total;
}

pub fn main(init: std.process.Init.Minimal) !void {
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();
    const stdout = std.Io.File.stdout();

    // Section 5.2 program modes: argv[1] in {once, steady}, argv[2] = K (default 7).
    var mode: []const u8 = "once";
    var k_count: usize = 7;
    var args = init.args.iterate();
    _ = args.next();
    if (args.next()) |a| {
        mode = a;
        if (args.next()) |b| k_count = std.fmt.parseInt(usize, b, 10) catch 7;
    }
    if (k_count < 1) k_count = 7;

    var total: u64 = 0;
    if (std.mem.eql(u8, mode, "steady")) {
        var ibuf: [64]u8 = undefined;
        for (0..k_count) |k| {
            // Section 5.2 monotonic clock (Zig 0.16: std.Io.Clock.awake, see notes below).
            const t0 = std.Io.Clock.awake.now(io);
            total = workload();
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(total);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            // stdout is written unbuffered, so every ITER line is flushed as it is produced.
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        total = workload();
    }

    var buf: [64]u8 = undefined;
    const line = try std.fmt.bufPrint(&buf, "MB01 {d}\n", .{total});
    try stdout.writeStreamingAll(io, line);
}
