const std = @import("std");

fn probe(allocator: std.mem.Allocator) !i32 {
// BEGIN PROBE F06.P1
var xs: std.ArrayList(i32) = .empty;
try xs.appendSlice(allocator, &.{ 1, 2, 3 });
const y = mid(xs);
return y;
}

fn mid(v: std.ArrayList(i32)) i32 {
    return v.items[1];
}
// END PROBE F06.P1

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
