const std = @import("std");

fn mapped(o: ?i32) i32 {
// BEGIN PROBE F12.P2
const h = struct {
    fn h(x: i32) i32 {
        return x + 1;
    }
}.h;
const p: ?i32 = if (o) |v| h(v) else null;
return p orelse 0;
// END PROBE F12.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{mapped(5)});
    try out.interface.flush();
}
