fn probe(allocator: @import("std").mem.Allocator) i32 {
// BEGIN PROBE F19.P1
const std = @import("std");
const S = struct {
    fn first() i32 {
        return 20;
    }
    fn second() i32 {
        return 22;
    }
};
var threaded: std.Io.Threaded = .init(allocator, .{});
defer threaded.deinit();
const io = threaded.io();
var f1 = io.async(S.first, .{});
var f2 = io.async(S.second, .{});
const sum = f1.await(io) + f2.await(io);
// END PROBE F19.P1
    return sum;
}

pub fn main() !void {
    const s = @import("std");
    var threaded: s.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = s.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe(s.heap.page_allocator)});
    try out.interface.flush();
}
