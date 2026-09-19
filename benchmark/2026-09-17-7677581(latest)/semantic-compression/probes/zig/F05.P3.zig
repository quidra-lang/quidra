const std = @import("std");

fn probe(allocator: std.mem.Allocator, xs: std.ArrayList(i32)) !i32 {
    const k: i32 = 0;
// BEGIN PROBE F05.P3
const neg = struct {
    fn call(a: i32) i32 {
        return k - a;
    }
}.call;
var ys: std.ArrayList(i32) = .empty;
for (xs.items) |e| try ys.append(allocator, neg(e));
return ys.items[0];
// END PROBE F05.P3
}

pub fn main() !void {
    const allocator = std.heap.page_allocator;
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 1, 2, 3 });
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(allocator, xs)});
    try out.interface.flush();
}
