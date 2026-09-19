const std = @import("std");

fn probe(allocator: std.mem.Allocator) !i32 {
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 4, 5, 6 });
// BEGIN PROBE F04.P2
const window: []const i32 = xs.items;
return window[0];
// END PROBE F04.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
