const std = @import("std");
pub fn main() !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    const a = gpa.allocator();
    var list: std.ArrayList(i32) = .empty;
    defer list.deinit(a);
    try list.append(a, 1); try list.append(a, 2); try list.append(a, 3);
    var map = std.StringHashMap(i32).init(a);
    defer map.deinit();
    try map.put("a", 1); try map.put("b", 2);
    var set = std.BufSet.init(a);
    defer set.deinit();
    try set.insert("x"); try set.insert("y"); try set.insert("z"); try set.insert("x");
    std.debug.print("X14 {d} {d} {d}\n", .{ list.items.len, map.get("b").?, set.count() });
}
