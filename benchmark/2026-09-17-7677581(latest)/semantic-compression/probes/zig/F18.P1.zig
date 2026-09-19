pub fn main() !void {
// BEGIN PROBE F18.P1
const std = @import("std");
var threaded: std.Io.Threaded = .init_single_threaded;
try std.Io.File.stdout().writeStreamingAll(threaded.io(), "x\n");
// END PROBE F18.P1
}
