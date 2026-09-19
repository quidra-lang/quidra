const std = @import("std");

fn probe(allocator: std.mem.Allocator) !i32 {
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 7, 8, 9 });
// BEGIN PROBE F03.P2
xs.items[1] = 42;
// END PROBE F03.P2
    return xs.items[1];
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
