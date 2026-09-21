// MB-05 - Vector inner product: straight-line dot product over two flat arrays.
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

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded to 20265917 and X and Y are regenerated on every iteration).
/// Section 4.12(a) pins zig's binary64 array to `[]f64` from `std.heap.c_allocator`.
fn workload(allocator: std.mem.Allocator) !f64 {
    const n = 2000000;
    const r = 400;

    const x = try allocator.alloc(f64, n);
    defer allocator.free(x);
    const y = try allocator.alloc(f64, n);
    defer allocator.free(y);

    var gen = Lcg{ .state = 20265917 };
    for (x) |*v| v.* = 0.5 + gen.next_unit();
    for (y) |*v| v.* = 0.5 + gen.next_unit();

    var total: f64 = 0.0;
    var round: usize = 0;
    while (round < r) : (round += 1) {
        x[round] = x[round] + 1.0e-9; // anti-elimination; r <= n so no wrap
        var d: f64 = 0.0;
        for (x, y) |xv, yv| {
            d = d + xv * yv;
        }
        total = total + d;
    }
    return total;
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

    var total: f64 = 0.0;
    if (std.mem.eql(u8, mode, "steady")) {
        var ibuf: [64]u8 = undefined;
        for (0..k_count) |k| {
            const t0 = std.Io.Clock.awake.now(io);
            total = try workload(allocator);
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(total);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        total = try workload(allocator);
    }

    var buf: [128]u8 = undefined;
    const line = try std.fmt.bufPrint(&buf, "MB05 total={e:.16}\n", .{total});
    try stdout.writeStreamingAll(io, line);
}
