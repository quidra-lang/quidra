// MB-11 - Collections: hash map, hash set and dynamic array under insert / update /
// lookup / delete / iterate, using the standard library containers.
const std = @import("std");

const n: usize = 1000000;
const rounds: usize = 3;
const km: i64 = 500009;
const sm: i64 = 100003;
const q: i64 = 1000000007;
const seed: i64 = 20271917;

/// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
const Lcg = struct {
    state: i64,

    fn nextInt(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod
        return self.state;
    }
};

const Result = struct {
    acc: i64,
    size1: i64,
    found: i64,
    vsum: i64,
    mchk: i64,
    size2: i64,
    size3: i64,
    lsum: i64,
};

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and `keys` is regenerated on every iteration).
/// Section 4.12(a)/(d) pin zig's containers: `[]i64` keys, `std.AutoHashMap(i64,i64)`,
/// `std.AutoHashMap(i64,void)`, `std.ArrayList(i64)`, all on `std.heap.c_allocator`.
fn workload(gpa: std.mem.Allocator) !Result {
    const keys = try gpa.alloc(i64, n);
    defer gpa.free(keys);
    var g = Lcg{ .state = seed };
    for (keys) |*e| e.* = g.nextInt();

    var acc: i64 = 0;
    var size1: i64 = 0;
    var found: i64 = 0;
    var vsum: i64 = 0;
    var mchk: i64 = 0;
    var size2: i64 = 0;
    var size3: i64 = 0;
    var lsum: i64 = 0;

    for (0..rounds) |r| {
        keys[r] += 1000000; // anti-elimination

        var m = std.AutoHashMap(i64, i64).init(gpa);
        defer m.deinit();
        for (keys) |key| {
            const k = @rem(key, km);
            const entry = try m.getOrPut(k);
            if (!entry.found_existing) entry.value_ptr.* = 0;
            entry.value_ptr.* += 1;
        }
        size1 = m.count();

        found = 0;
        vsum = 0;
        for (keys) |key| {
            const k = @rem(key + 7, km);
            if (m.get(k)) |v| {
                found += 1;
                vsum += v;
            }
        }

        mchk = 0;
        var it = m.iterator(); // any iteration order
        while (it.next()) |entry| {
            mchk = @rem(mchk + @rem(entry.key_ptr.*, 1000003) * entry.value_ptr.*, 1000003);
        }

        var i: usize = 0;
        while (i < n) : (i += 2) {
            _ = m.remove(@rem(keys[i], km));
        }
        size2 = m.count();

        var st = std.AutoHashMap(i64, void).init(gpa);
        defer st.deinit();
        for (keys) |key| try st.put(@rem(key, sm), {});
        size3 = st.count();

        var lst: std.ArrayList(i64) = .empty;
        defer lst.deinit(gpa);
        for (keys) |key| try lst.append(gpa, @rem(key, 1000));
        lsum = 0;
        for (lst.items) |v| lsum += v;

        for ([_]i64{ size1, found, vsum, mchk, size2, size3, lsum }) |v| {
            acc = @rem(acc * 31 + v, q);
        }
    }
    return .{
        .acc = acc,
        .size1 = size1,
        .found = found,
        .vsum = vsum,
        .mchk = mchk,
        .size2 = size2,
        .size3 = size3,
        .lsum = lsum,
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

    var out: [256]u8 = undefined;
    const line = try std.fmt.bufPrint(
        &out,
        "MB11 {d} {d} {d} {d} {d} {d} {d} {d}\n",
        .{ res.acc, res.size1, res.found, res.vsum, res.mchk, res.size2, res.size3, res.lsum },
    );
    try stdout.writeStreamingAll(io, line);
}
