const std = @import("std");
fn makeAdder(n: i32) fn (i32) i32 { return struct { fn add(x: i32) i32 { return x + n; } }.add; }
pub fn main() !void {
    var threaded: std.Io.Threaded = .init_single_threaded; _ = &threaded;
    var n: i32 = 10; n += 0;                 // runtime value
    const f = makeAdder(n);
    std.debug.print("X05 {d}\n", .{f(5)});
}
