const std = @import("std");

var src: i32 = 1;

fn probe(allocator: std.mem.Allocator) !struct { i32, bool } {
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 1, 3, 2 });
    const a: i32 = src;
    const b: i32 = 2;
// BEGIN PROBE F09.P2
std.mem.sort(i32, xs.items, {}, std.sort.desc(i32));
const lt = a < b;
// END PROBE F09.P2
    return .{ xs.items[0], lt };
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    const t = try probe(std.heap.page_allocator);
    try out.interface.print("{d} {}\n", .{ t[0], t[1] });
    try out.interface.flush();
}
