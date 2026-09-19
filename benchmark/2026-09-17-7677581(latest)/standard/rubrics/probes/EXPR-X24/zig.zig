const std = @import("std");
pub fn main() !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    const a = gpa.allocator();
    var v = try std.math.big.int.Managed.initSet(a, 2);
    defer v.deinit();
    try v.pow(&v, 70);
    const s = try v.toString(a, 10, .lower);
    defer a.free(s);
    std.debug.print("X24 {s}\n", .{s});
}
