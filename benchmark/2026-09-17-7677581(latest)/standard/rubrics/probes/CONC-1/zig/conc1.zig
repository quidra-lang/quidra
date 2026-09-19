// CONC-1, Zig. Worker mechanism: std.Thread (Zig standard library).
const std = @import("std");

const N: i64 = 500000000;
const CHUNKS: usize = 4;
const SPAN: i64 = N / @as(i64, CHUNKS);

var partial: [CHUNKS]f64 = .{0.0} ** CHUNKS;

fn chunkSum(c: usize) f64 {
    var s: f64 = 0.0;
    const start: i64 = @as(i64, @intCast(c)) * SPAN;
    const end: i64 = start + SPAN;
    var i: i64 = start;
    while (i < end) : (i += 1) {
        const x = @sin(@as(f64, @floatFromInt(i)));
        s += x * x;
    }
    return s;
}

fn worker(w: usize, total_workers: usize) void {
    var c: usize = 0;
    while (c < CHUNKS) : (c += 1) {
        if (c % total_workers == w) partial[c] = chunkSum(c);
    }
}

pub fn main(init: std.process.Init.Minimal) !void {
    var it: std.process.Args.Iterator = .init(init.args);
    _ = it.skip();
    const w_count: usize = if (it.next()) |a| try std.fmt.parseInt(usize, a, 10) else 1;

    var threads: [8]std.Thread = undefined;
    var w: usize = 0;
    while (w < w_count) : (w += 1) {
        threads[w] = try std.Thread.spawn(.{}, worker, .{ w, w_count });
    }
    w = 0;
    while (w < w_count) : (w += 1) threads[w].join();

    var total: f64 = 0.0;
    var c: usize = 0;
    while (c < CHUNKS) : (c += 1) total = total + partial[c];
    std.debug.print("workers={d} result={d:.10}\n", .{ w_count, total });
}
