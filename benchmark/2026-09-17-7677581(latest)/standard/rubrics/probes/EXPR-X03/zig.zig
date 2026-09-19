const std = @import("std");
fn Box(comptime T: type) type { return struct { v: T, fn get(self: @This()) T { return self.v; } }; }
pub fn main() void {
    const a = Box(i32){ .v = 5 };
    const b = Box([]const u8){ .v = "hi" };
    std.debug.print("X03 {d} {s}\n", .{ a.get(), b.get() });
}
