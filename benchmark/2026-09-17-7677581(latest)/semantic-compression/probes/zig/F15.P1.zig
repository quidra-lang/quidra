const std = @import("std");

fn combine(allocator: std.mem.Allocator) ![]u8 {
// BEGIN PROBE F15.P1
const head = struct {
    fn head(comptime T: type, xs: []const T) T {
        return xs[0];
    }
}.head;
const a = head(i32, &.{ 4, 5, 6 });
const b = head([]const u8, &.{ "p", "q" });
return std.fmt.allocPrint(allocator, "{d}{s}", .{ a, b });
// END PROBE F15.P1
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{s}\n", .{try combine(std.heap.page_allocator)});
    try out.interface.flush();
}
