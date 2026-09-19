const std = @import("std");
const shapes = @import("shapes.zig");

fn tri_area() f64 {
// BEGIN PROBE F14.P3
const Tri = struct {
    b: f64,
    h: f64,
    fn area(p: *const anyopaque) f64 {
        const self: *const @This() = @ptrCast(@alignCast(p));
        return 0.5 * self.b * self.h;
    }
};
const t: Tri = .{ .b = 3.0, .h = 4.0 };
const s: shapes.Shape = .{ .ptr = &t, .areaFn = Tri.area };
return s.area();
// END PROBE F14.P3
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{tri_area()});
    try out.interface.flush();
}
