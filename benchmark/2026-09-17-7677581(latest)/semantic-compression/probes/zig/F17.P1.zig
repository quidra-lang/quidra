const std = @import("std");

var io: std.Io = undefined;

// BEGIN PROBE F17.P1
fn readAll() !usize {
    const file = try std.Io.Dir.cwd().openFile(io, "data.txt", .{});
    defer file.close(io);
    var reader = file.reader(io, &.{});
    const text = try reader.interface.allocRemaining(std.heap.page_allocator, .unlimited);
    return text.len;
}
// END PROBE F17.P1

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{try readAll()});
    try out.interface.flush();
}
