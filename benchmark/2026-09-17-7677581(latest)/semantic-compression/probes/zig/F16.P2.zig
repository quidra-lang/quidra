const std = @import("std");

fn probe(allocator: std.mem.Allocator) !i32 {
// BEGIN PROBE F16.P2
var mp = std.StringHashMap(i32).init(allocator);
try mp.put("a", 1);
var total: i32 = 0;
var it = mp.iterator();
while (it.next()) |e| total += e.value_ptr.*;
const miss = mp.get("b") orelse 0;
return total + miss;
// END PROBE F16.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
