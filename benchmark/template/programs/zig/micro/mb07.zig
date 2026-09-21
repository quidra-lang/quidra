// MB-07 - Sorting: bottom-up iterative merge sort, ascending, stable, ping-pong buffers.
const std = @import("std");

const n: usize = 2000000;
const rounds: usize = 4;
const seed: i64 = 20267917;

/// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
const Lcg = struct {
    state: i64,

    fn nextInt(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod (section 2.1)
        return self.state;
    }
};

fn msort(a: []i64, buf: []i64) void {
    var src = a;
    var dst = buf;

    var width: usize = 1;
    while (width < a.len) : (width *= 2) {
        var lo: usize = 0;
        while (lo < a.len) : (lo += 2 * width) {
            const mid = @min(lo + width, a.len);
            const hi = @min(lo + 2 * width, a.len);
            var i = lo;
            var j = mid;
            var k = lo;
            while (i < mid and j < hi) : (k += 1) {
                if (src[i] <= src[j]) {
                    dst[k] = src[i];
                    i += 1;
                } else {
                    dst[k] = src[j];
                    j += 1;
                }
            }
            while (i < mid) : ({
                i += 1;
                k += 1;
            }) dst[k] = src[i];
            while (j < hi) : ({
                j += 1;
                k += 1;
            }) dst[k] = src[j];
        }
        std.mem.swap([]i64, &src, &dst);
    }

    if (src.ptr != a.ptr) @memcpy(a, src);
}

const Result = struct { total: i64, ssum: i64, inv: i64 };

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and `src` is regenerated on every iteration).
/// Section 4.12(a) pins zig's 64-bit integer array to `[]i64` from `std.heap.c_allocator`.
fn workload(gpa: std.mem.Allocator) !Result {
    const src = try gpa.alloc(i64, n);
    defer gpa.free(src);
    var g = Lcg{ .state = seed };
    for (src) |*e| e.* = g.nextInt();

    const buf = try gpa.alloc(i64, n);
    defer gpa.free(buf);

    var total: i64 = 0;
    var ssum: i64 = 0;
    var inv: i64 = 0;

    for (0..rounds) |r| {
        src[r] += 1; // anti-elimination
        const a = try gpa.dupe(i64, src); // `a = copy of src`, fresh every round
        defer gpa.free(a);
        msort(a, buf);

        var chk: i64 = 0;
        for (a) |v| chk = @rem(chk * 31 + @rem(v, 1000003), 1000003);
        total = @rem(total * 7 + chk, 1000003);

        ssum = 0;
        for (a) |v| ssum += v;

        for (1..n) |i| {
            if (a[i - 1] > a[i]) inv += 1;
        }
    }
    return .{ .total = total, .ssum = ssum, .inv = inv };
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

    var out: [128]u8 = undefined;
    const line = try std.fmt.bufPrint(&out, "MB07 {d} {d} {d}\n", .{ res.total, res.ssum, res.inv });
    try stdout.writeStreamingAll(io, line);
}
