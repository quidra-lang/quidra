const std = @import("std");
const Person = struct { name: []const u8, age: u32 };
pub fn main() !void {
    var gpa = std.heap.DebugAllocator(.{}){};
    const a = gpa.allocator();
    const s = try std.json.Stringify.valueAlloc(a, Person{ .name = "alice", .age = 30 }, .{});
    defer a.free(s);
    const parsed = try std.json.parseFromSlice(Person, a, s, .{});
    defer parsed.deinit();
    std.debug.print("X20 {s} {d}\n", .{ parsed.value.name, parsed.value.age });
}
