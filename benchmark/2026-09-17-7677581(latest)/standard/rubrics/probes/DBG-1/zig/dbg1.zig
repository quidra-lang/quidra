// DBG-1
const std = @import("std");

const ITERATIONS: i64 = 300000000;
const MODULUS: i64 = 1000000007;

const Item = struct {
    count: i64,
    name: []const u8,
};

fn accumulate(items: []const Item, iterations: i64) i64 {
    var total: i64 = 0;
    var i: i64 = 0;
    while (i < iterations) : (i += 1) {
        for (items) |item| {
            total = @rem(total * 31 + item.count + @as(i64, @intCast(item.name.len)), MODULUS);
        }
    }
    return total;
}

pub fn main() !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    const allocator = gpa.allocator();
    var items: std.ArrayList(Item) = .empty;
    defer items.deinit(allocator);
    try items.append(allocator, Item{ .count = 7, .name = "alpha" });
    try items.append(allocator, Item{ .count = 11, .name = "bravo" });
    try items.append(allocator, Item{ .count = 13, .name = "charlie" });
    const checksum = accumulate(items.items, ITERATIONS);
    std.debug.print("checksum={d}\n", .{checksum});
}
