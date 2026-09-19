const std = @import("std");
pub fn main() void { var buf = [_]i32{ 0, 0, 0, 0 };
    const view: []i32 = buf[1..3];
    view[0] = 99; view[1] = 99;
    std.debug.print("X23 {d} {d}\n", .{ buf[1], buf[2] }); }
