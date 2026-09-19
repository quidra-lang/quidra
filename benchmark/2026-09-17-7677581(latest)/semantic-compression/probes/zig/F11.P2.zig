const std = @import("std");

fn second(xs: std.ArrayList(i32)) i32 {
// BEGIN PROBE F11.P2
const part = xs.items[1..4];
return part[0];
// END PROBE F11.P2
}

pub fn main() !void {
    const allocator = std.heap.page_allocator;
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 10, 20, 30, 40, 50 });
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{second(xs)});
    try out.interface.flush();
}
