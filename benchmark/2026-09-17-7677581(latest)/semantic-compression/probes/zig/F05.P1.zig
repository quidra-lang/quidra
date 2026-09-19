const std = @import("std");

var seen: i32 = 0;

fn f(v: std.ArrayList(i32)) void {
    seen = v.items[0];
}

fn probe(allocator: std.mem.Allocator) !i32 {
// BEGIN PROBE F05.P1
var x: std.ArrayList(i32) = .empty;
try x.appendSlice(allocator, &.{ 1, 2, 3 });
f(x);
return x.items[0];
// END PROBE F05.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
