const std = @import("std");

const Node = struct {
    id: i32,
};

fn probe(allocator: std.mem.Allocator) !bool {
// BEGIN PROBE F04.P3
const first = try allocator.create(Node);
first.id = 5;
const box = [1]*Node{first};
const second = box[0];
const same = first == second;
return same;
// END PROBE F04.P3
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{}\n", .{try probe(std.heap.page_allocator)});
    try out.interface.flush();
}
