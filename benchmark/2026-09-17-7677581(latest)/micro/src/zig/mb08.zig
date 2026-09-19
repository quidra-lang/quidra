// MB-08 - Strings: text construction plus five character-level passes per round.
const std = @import("std");

const word_count: usize = 200000;
const rounds: usize = 20;
const q: i64 = 1000000007;
const seed: i64 = 20268917;

/// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
const Lcg = struct {
    state: i64,

    fn nextInt(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod
        return self.state;
    }
};

/// Rolling hash, order-sensitive.
fn rhash(seq: []const u8) i64 {
    var h: i64 = 0;
    for (seq) |c| h = @rem(h * 131 + c, q);
    return h;
}

const Result = struct { acc: i64, len: usize, cnt_ab: i64, cnt_w: i64 };

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and the text is rebuilt on every iteration).
/// Section 4.12(b) pins zig's working buffer to `[]u8` from `std.heap.c_allocator`
/// and zig's "native string" to `[]const u8` read with `s[i]`.
fn workload(gpa: std.mem.Allocator) !Result {
    var g = Lcg{ .state = seed };

    // Build phase: the pinned two-phase build -- fill `words`, then join with " ".
    const words = try gpa.alloc([]const u8, word_count);
    defer gpa.free(words);
    for (words) |*w| {
        const wlen: usize = @intCast(4 + @rem(g.nextInt(), 13)); // length 4..16
        const word = try gpa.alloc(u8, wlen);
        for (word) |*c| c.* = @intCast('a' + @rem(g.nextInt(), 26));
        w.* = word;
    }
    const text = try std.mem.join(gpa, " ", words); // language's normal join
    defer gpa.free(text);
    for (words) |w| gpa.free(w);
    const len = text.len;

    var acc: i64 = 0;
    var cnt_ab: i64 = 0;
    var cnt_w: i64 = 0;

    for (0..rounds) |r| {
        const p = 7 * r + 11; // anti-elimination
        if (text[p] == ' ') {
            text[p] = 'x';
        } else {
            text[p] = 'a' + (text[p] - 'a' + 1) % 26;
        }

        const h1 = rhash(text); // pass 1

        const upper = try gpa.alloc(u8, len); // pass 2: upper-case
        defer gpa.free(upper);
        for (upper, text) |*u, c| u.* = if (c >= 97 and c <= 122) c - 32 else c;
        const h2 = rhash(upper);

        const reversed = try gpa.alloc(u8, len); // pass 3: reverse
        defer gpa.free(reversed);
        for (reversed, 0..) |*v, i| v.* = text[len - 1 - i];
        const h3 = rhash(reversed);

        cnt_ab = 0; // pass 4: naive search
        for (0..len - 1) |i| {
            if (text[i] == 'a' and text[i + 1] == 'b') cnt_ab += 1;
        }

        // Pass 5 runs on the language's own string value, constructed fresh from
        // the working buffer inside the timed round and read with `s[i]`
        // (section 4.12(b)). Zig has no distinct string type: its string value is
        // `[]const u8`, so the construction is a slice coercion, not a copy.
        const s: []const u8 = text;
        cnt_w = 1;
        for (0..len) |i| {
            if (s[i] == ' ') cnt_w += 1;
        }

        for ([_]i64{ h1, h2, h3, cnt_ab, cnt_w }) |v| acc = @rem(acc * 31 + v, q);
    }
    return .{ .acc = acc, .len = len, .cnt_ab = cnt_ab, .cnt_w = cnt_w };
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
    const line = try std.fmt.bufPrint(&out, "MB08 {d} {d} {d} {d}\n", .{ res.acc, res.len, res.cnt_ab, res.cnt_w });
    try stdout.writeStreamingAll(io, line);
}
