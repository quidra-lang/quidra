const std = @import("std");
pub fn main() !void {
    const allocator = std.heap.page_allocator;
    var xs: std.ArrayList(i32) = .empty;
    try xs.appendSlice(allocator, &.{ 10, 20, 30, 40, 50 });
    const part = xs.items[1..4];
    xs.items[1] = 99;
    std.debug.print("{d}\n", .{part[0]});
}
