// MB-10 - File I/O: buffered text write, read-back, integer formatting and parsing.
const std = @import("std");

const n: u64 = 1000000;
const rounds: usize = 3;
const seed: i64 = 20270917;

/// Lehmer / MINSTD generator: s = (48271 * s) mod (2^31 - 1).
const Lcg = struct {
    state: i64,

    fn nextInt(self: *Lcg) i64 {
        self.state = @rem(48271 * self.state, 2147483647); // operands non-negative: rem == mod
        return self.state;
    }
};

const Result = struct { sum_v: i64, chk: i64, nbytes: u64, lines: u64 };

/// The whole workload body (section 5.2: `steady` re-runs this, so the generator
/// is re-seeded and the files are rewritten on every iteration).
/// Section 4.12(c) pins a 65536-byte explicit buffer on both the writer and the reader.
fn workload(io: std.Io, cwd: std.Io.Dir) !Result {
    var g = Lcg{ .state = seed }; // the stream continues across rounds
    var sum_v: i64 = 0;
    var chk: i64 = 0;
    var nbytes: u64 = 0;
    var lines: u64 = 0;

    for (0..rounds) |r| {
        var name_buf: [32]u8 = undefined;
        const name = try std.fmt.bufPrint(&name_buf, "mb10_round_{d}.txt", .{r});

        var write_buf: [64 * 1024]u8 = undefined;
        {
            const file = try cwd.createFile(io, name, .{});
            defer file.close(io);
            var file_writer = file.writer(io, &write_buf);
            const w = &file_writer.interface;
            for (0..n) |i| {
                const v = g.nextInt();
                var line_buf: [32]u8 = undefined;
                const line = try std.fmt.bufPrint(&line_buf, "{d} {d}\n", .{ i, v });
                try w.writeAll(line);
                nbytes += line.len;
            }
            try file_writer.end();
        }

        var read_buf: [64 * 1024]u8 = undefined;
        {
            const file = try cwd.openFile(io, name, .{});
            defer file.close(io);
            var file_reader = file.reader(io, &read_buf);
            const rd = &file_reader.interface;
            var idx: u64 = 0;
            while (try rd.takeDelimiter('\n')) |line| {
                const space = std.mem.indexOfScalar(u8, line, ' ').?;
                const a = try std.fmt.parseInt(u64, line[0..space], 10);
                const v = try std.fmt.parseInt(i64, line[space + 1 ..], 10);
                if (a != idx) return error.IndexMismatch;
                idx += 1;
                lines += 1;
                sum_v = @rem(sum_v + v, 1000000007);
                chk = @rem(chk * 31 + @rem(v, 1000003), 1000003);
            }
        }
    }
    return .{ .sum_v = sum_v, .chk = chk, .nbytes = nbytes, .lines = lines };
}

pub fn main(init: std.process.Init.Minimal) !void {
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();
    const cwd = std.Io.Dir.cwd();
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
            res = try workload(io, cwd);
            const t1 = std.Io.Clock.awake.now(io);
            std.mem.doNotOptimizeAway(res);
            const ns: u64 = @intCast(t0.durationTo(t1).nanoseconds);
            const iter_line = try std.fmt.bufPrint(&ibuf, "ITER {d} {d}\n", .{ k, ns });
            try stdout.writeStreamingAll(io, iter_line);
        }
    } else {
        res = try workload(io, cwd);
    }

    var out: [128]u8 = undefined;
    const line = try std.fmt.bufPrint(&out, "MB10 {d} {d} {d} {d}\n", .{ res.sum_v, res.chk, res.nbytes, res.lines });
    try stdout.writeStreamingAll(io, line);
}
