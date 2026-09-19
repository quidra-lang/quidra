// CONC-2: two concurrent workers increment a shared counter with NO synchronization.
const std = @import("std");

const ITERS: i64 = 200000;
var counter: i64 = 0;

fn bump() void {
    var i: i64 = 0;
    while (i < ITERS) : (i += 1) counter = counter + 1;
}

pub fn main() !void {
    const t1 = try std.Thread.spawn(.{}, bump, .{});
    const t2 = try std.Thread.spawn(.{}, bump, .{});
    t1.join();
    t2.join();
    std.debug.print("counter={d} expected={d}\n", .{ counter, 2 * ITERS });
}
