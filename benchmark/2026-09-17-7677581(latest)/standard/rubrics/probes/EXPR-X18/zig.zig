const std = @import("std");
pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var gpa = std.heap.DebugAllocator(.{}){};
    const a = gpa.allocator();
    const cwd = std.Io.Dir.cwd();
    try cwd.writeFile(io, .{ .sub_path = "x18.txt", .data = "hello" });
    const data = try cwd.readFileAlloc(io, "x18.txt", a, .limited(1024));
    defer a.free(data);
    const exists = if (cwd.statFile(io, "x18.txt", .{})) |_| true else |_| false;
    std.debug.print("X18 {s} {}\n", .{ data, exists });
}
