const std = @import("std");

const Shape = union(enum) { circle: struct { r: f64 }, rect: struct { w: f64, h: f64 } };

fn area_of(s: Shape) f64 {
// BEGIN PROBE F14.P2
const area: f64 = switch (s) {
    .circle => |c| 3.141592653589793 * c.r * c.r,
};
return area;
// END PROBE F14.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{area_of(.{ .circle = .{ .r = 2.0 } })});
    try out.interface.flush();
}
