// MB-03 - Integer arithmetic: mixed add / xor / multiply-modulo / divide.
const std = @import("std");

const Result = struct { s_add: i64, s_xor: i64, s_mul: i64, s_div: i64 };

/// The whole workload body (section 5.2: this is what `steady` re-runs; the
/// generator state is re-seeded here on every call).
fn workload() Result {
    const n: u64 = 120000000;

    // Section 2.1: every operand here is non-negative, so @rem == @mod and
    // @divTrunc == floor division; the 64-bit signed type is the one the other
    // nine configurations use for this kernel.
    var x: i64 = 20263917; // the seed itself is the initial generator state
    var s_add: i64 = 0;
    var s_xor: i64 = 0;
    var s_mul: i64 = 1;
    var s_div: i64 = 0;

    var i: u64 = 0;
    while (i < n) : (i += 1) {
        x = @rem(48271 * x, 2147483647);
        s_add = @rem(s_add + x, 2147483647);
        s_xor = s_xor ^ x;
        s_mul = @rem(s_mul * 33 + @rem(x, 97), 1000003);
        s_div = s_div + @divTrunc(x, 1000);
    }
    return .{ .s_add = s_add, .s_xor = s_xor, .s_mul = s_mul, .s_div = s_div };
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

    var res: Result = undefined;
    if (std.mem.eql(u8, mode, "steady")) {
        var ibuf: [64]u8 = undefined;
        for (0..k_count) |k| {
            const t0 = std.Io.Clock.awake.now(io);
            res = workload();
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(res);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        res = workload();
    }

    var buf: [128]u8 = undefined;
    const line = try std.fmt.bufPrint(&buf, "MB03 {d} {d} {d} {d}\n", .{ res.s_add, res.s_xor, res.s_mul, res.s_div });
    try stdout.writeStreamingAll(io, line);
}
