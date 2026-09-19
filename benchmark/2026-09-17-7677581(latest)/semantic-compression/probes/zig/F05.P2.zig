const std = @import("std");

fn probe() i32 {
// BEGIN PROBE F05.P2
const put = struct {
    fn call(out: *i32) void {
        out.* = 12;
    }
}.call;
var cell: i32 = 3;
put(&cell);
return cell;
// END PROBE F05.P2
}

pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var out_buf: [64]u8 = undefined;
    var out = std.Io.File.stdout().writer(io, &out_buf);
    try out.interface.print("{d}\n", .{probe()});
    try out.interface.flush();
}
