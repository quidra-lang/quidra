const std = @import("std");

var released: i32 = 0;

fn probe() i32 {
// BEGIN PROBE F17.P2
const Handle = struct {
    id: i32,
    fn deinit(_: @This()) void {
        released += 1;
    }
};

{
    const h = Handle{ .id = 1 };
    defer h.deinit();
}

const result = released;
// END PROBE F17.P2
    return result;
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
