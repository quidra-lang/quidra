const std = @import("std");

fn first_tag() []const u8 {
// BEGIN PROBE F15.P3
const Named = struct {
    ptr: *const anyopaque,
    tagFn: *const fn (*const anyopaque) []const u8,
    fn tag(self: @This()) []const u8 {
        return self.tagFn(self.ptr);
    }
};
const A = struct {
    fn tag(_: *const anyopaque) []const u8 {
        return "a";
    }
};
const B = struct {
    fn tag(_: *const anyopaque) []const u8 {
        return "b";
    }
};
const a: A = .{};
const b: B = .{};
const items = [_]Named{ .{ .ptr = &a, .tagFn = A.tag }, .{ .ptr = &b, .tagFn = B.tag } };
return items[0].tag();
// END PROBE F15.P3
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{s}\n", .{first_tag()});
    try out.interface.flush();
}
