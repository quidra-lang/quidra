const std = @import("std");

var src: i32 = 1;

fn probe(allocator: std.mem.Allocator) !bool {
    const s1 = try std.fmt.allocPrint(allocator, "ab{d}", .{src});
    const s2 = try std.fmt.allocPrint(allocator, "ab{d}", .{src});
// BEGIN PROBE F09.P1
const eq = std.mem.eql(u8, s1, s2);
// END PROBE F09.P1
    return eq;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
