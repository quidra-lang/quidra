const std = @import("std");

pub fn main() !void {
    var t = std.Io.Threaded.init_single_threaded;
    const io = t.io();
    const out = std.Io.File.stdout();
    try out.writeStreamingAll(io, "ADV-START\n");

    const gpa = std.heap.page_allocator;
    const s: []const u8 = try std.Io.Dir.cwd().readFileAlloc(io, "inputs/ADV-23.bin", gpa, .unlimited);

    var ob: [64]u8 = undefined;
    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=CP:{d}\n", .{s.len}));
    try out.writeStreamingAll(io, "ADV-END\n");
}
