const std = @import("std");

pub fn main(init: std.process.Init.Minimal) !void {
    _ = init;
    var threaded = std.Io.Threaded.init_single_threaded;
    const io = threaded.io();
    const stdout = std.Io.File.stdout();
    try stdout.writeStreamingAll(io, "HELLO\n");
}
