const std = @import("std");

fn read_at(xs: std.ArrayList(i32), i: i32) i32 {
// BEGIN PROBE F11.P1
const e = xs.items[@intCast(i)];
return e;
// END PROBE F11.P1
}

pub fn main() !void {
    const allocator = std.heap.page_allocator;
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 1, 2, 3 });
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{read_at(xs, 2)});
    try out.interface.flush();
}
