// MB-04 - Floating-point arithmetic: four accumulators over two arrays.
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

const Result = struct { s1: f64, s2: f64, s3: f64, s4: f64 };

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and the input arrays are regenerated on every iteration).
/// Section 4.12(a) pins zig's binary64 array to `[]f64` from `std.heap.c_allocator`.
fn workload(allocator: std.mem.Allocator) !Result {
    const m = 4000;
    const r = 75000;

    const a = try allocator.alloc(f64, m);
    defer allocator.free(a);
    const b = try allocator.alloc(f64, m);
    defer allocator.free(b);

    var gen = Lcg{ .state = 20264917 };
    for (a) |*v| v.* = 0.5 + gen.next_unit();
    for (b) |*v| v.* = 0.5 + gen.next_unit();

    var s1: f64 = 0.0;
    var s2: f64 = 0.0;
    var s3: f64 = 0.0;
    var s4: f64 = 0.0;

    var round: usize = 0;
    while (round < r) : (round += 1) {
        a[round % m] = a[round % m] + 1.0e-9; // anti-elimination, part of the algorithm
        for (a, b) |av, bv| {
            s1 = s1 + av * bv;
            s2 = s2 + av / (bv + 2.0);
            s3 = s3 + @sqrt(av * av + bv * bv);
            s4 = s4 + (av - bv) * (av - bv);
        }
    }
    return .{ .s1 = s1, .s2 = s2, .s3 = s3, .s4 = s4 };
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
        "MB04 s1={e:.16} s2={e:.16} s3={e:.16} s4={e:.16}\n",
        .{ res.s1, res.s2, res.s3, res.s4 },
    );
    try stdout.writeStreamingAll(io, line);
}
