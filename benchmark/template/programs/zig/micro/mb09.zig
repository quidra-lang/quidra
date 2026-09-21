// MB-09 - Statistics: two-pass moments, Pearson correlation, histogram.
const std = @import("std");

const n: usize = 2000000;
const rounds: usize = 30;
const seed: i64 = 20269917;

/// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
const Lcg = struct {
    state: i64,

    fn nextInt(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod
        return self.state;
    }

    fn nextUnit(self: *Lcg) f64 {
        return @as(f64, @floatFromInt(self.nextInt())) / 2147483647.0;
    }
};

const Result = struct {
    mean: f64,
    variance: f64,
    sd: f64,
    mn: f64,
    mx: f64,
    mad: f64,
    pearson: f64,
    hist_chk: u64,
};

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and X and Y are regenerated on every iteration).
/// Section 4.12(a) pins zig's binary64 array to `[]f64` from `std.heap.c_allocator`.
fn workload(gpa: std.mem.Allocator) !Result {
    const nf: f64 = @floatFromInt(n);

    const x = try gpa.alloc(f64, n);
    defer gpa.free(x);
    const y = try gpa.alloc(f64, n);
    defer gpa.free(y);

    var g = Lcg{ .state = seed };
    for (x) |*e| e.* = g.nextUnit() * 100.0;
    for (y) |*e| e.* = g.nextUnit() * 100.0;

    var mean: f64 = 0.0;
    var variance: f64 = 0.0;
    var sd: f64 = 0.0;
    var mn: f64 = 0.0;
    var mx: f64 = 0.0;
    var mad: f64 = 0.0;
    var pearson: f64 = 0.0;
    var hist_chk: u64 = 0;

    for (0..rounds) |r| {
        x[r] += 1.0e-9; // anti-elimination

        var s: f64 = 0.0; // pass 1
        mn = x[0];
        mx = x[0];
        for (x) |v| {
            s = s + v;
            if (v < mn) mn = v;
            if (v > mx) mx = v;
        }
        mean = s / nf;

        var sq: f64 = 0.0; // pass 2
        var ad: f64 = 0.0;
        for (x) |v| {
            const d = v - mean;
            sq = sq + d * d;
            ad = ad + if (d < 0) -d else d;
        }
        variance = sq / nf;
        sd = @sqrt(variance);
        mad = ad / nf;

        var sy: f64 = 0.0; // pass 3
        for (y) |v| sy = sy + v;
        const meany = sy / nf;

        var sxy: f64 = 0.0; // pass 4
        var sxx: f64 = 0.0;
        var syy: f64 = 0.0;
        for (x, y) |xv, yv| {
            const dx = xv - mean;
            const dy = yv - meany;
            sxy = sxy + dx * dy;
            sxx = sxx + dx * dx;
            syy = syy + dy * dy;
        }
        pearson = sxy / @sqrt(sxx * syy);

        var hist = [_]u64{0} ** 64; // pass 5
        for (x) |v| {
            var b: i64 = @intFromFloat(@floor(v * 0.64));
            if (b < 0) b = 0;
            if (b > 63) b = 63;
            hist[@intCast(b)] += 1;
        }
        hist_chk = 0;
        for (hist, 0..) |count, b| hist_chk += (b + 1) * count;
    }
    return .{
        .mean = mean,
        .variance = variance,
        .sd = sd,
        .mn = mn,
        .mx = mx,
        .mad = mad,
        .pearson = pearson,
        .hist_chk = hist_chk,
    };
}

pub fn main(init: std.process.Init.Minimal) !void {
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();
    const stdout = std.Io.File.stdout();
    const gpa = std.heap.c_allocator;

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
            res = try workload(gpa);
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(res);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        res = try workload(gpa);
    }

    var out: [512]u8 = undefined;
    const line = try std.fmt.bufPrint(
        &out,
        "MB09 mean={e:.16} var={e:.16} sd={e:.16} min={e:.16} max={e:.16} mad={e:.16} pearson={e:.16} hist_chk={d}\n",
        .{ res.mean, res.variance, res.sd, res.mn, res.mx, res.mad, res.pearson, res.hist_chk },
    );
    try stdout.writeStreamingAll(io, line);
}
