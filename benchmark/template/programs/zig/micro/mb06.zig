// MB-06 - Matrix multiplication: classical i-j-k order over flat row-major storage.
const std = @import("std");

/// Lehmer / MINSTD generator, frozen for every language in the suite.
const Lcg = struct {
    state: i64,

    fn next_int(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod (section 2.1)
        return self.state;
    }

    fn next_unit(self: *Lcg) f64 {
        const v: f64 = @floatFromInt(self.next_int());
        return v / 2147483647.0;
    }
};

const Result = struct { sum_c: f64, c_first: f64, c_last: f64 };

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and A and B are regenerated on every iteration).
/// Section 4.12(a) pins zig's binary64 array to `[]f64` from `std.heap.c_allocator`.
fn workload(allocator: std.mem.Allocator) !Result {
    const n = 512;
    const r = 3;

    const a = try allocator.alloc(f64, n * n);
    defer allocator.free(a);
    const b = try allocator.alloc(f64, n * n);
    defer allocator.free(b);
    const c = try allocator.alloc(f64, n * n);
    defer allocator.free(c);

    var gen = Lcg{ .state = 20266917 };
    for (a) |*v| v.* = gen.next_unit();
    for (b) |*v| v.* = gen.next_unit();

    var round: usize = 0;
    while (round < r) : (round += 1) {
        a[round] = a[round] + 1.0e-9; // anti-elimination, part of the algorithm
        var i: usize = 0;
        while (i < n) : (i += 1) {
            var j: usize = 0;
            while (j < n) : (j += 1) {
                var s: f64 = 0.0;
                var k: usize = 0;
                while (k < n) : (k += 1) {
                    s = s + a[i * n + k] * b[k * n + j];
                }
                c[i * n + j] = s;
            }
        }
    }

    var sum_c: f64 = 0.0;
    for (c) |v| {
        sum_c = sum_c + v;
    }
    return .{ .sum_c = sum_c, .c_first = c[0], .c_last = c[n * n - 1] };
}

pub fn main(init: std.process.Init.Minimal) !void {
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();
    const stdout = std.Io.File.stdout();
    const allocator = std.heap.c_allocator;

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
            res = try workload(allocator);
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(res);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        res = try workload(allocator);
    }

    var buf: [256]u8 = undefined;
    const line = try std.fmt.bufPrint(
        &buf,
        "MB06 sumC={e:.16} c_first={e:.16} c_last={e:.16}\n",
        .{ res.sum_c, res.c_first, res.c_last },
    );
    try stdout.writeStreamingAll(io, line);
}
