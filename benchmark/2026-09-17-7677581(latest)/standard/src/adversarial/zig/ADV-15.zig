const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const gpa = std.heap.page_allocator;
    var xs: std.ArrayList(i64) = .empty;
    try xs.appendSlice(gpa, &[_]i64{ 1, 2, 3, 4, 5 });

    var iters: i64 = 0;
    for (xs.items) |x| {
        iters += 1;
        if (x == 2) try xs.append(gpa, 99);
    }

    var ob: [128]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=ITERS:{d}|LEN:{d}\n", .{ iters, xs.items.len }));
    try out.writeStreamingAll(io, "ADV-END\n");
}
