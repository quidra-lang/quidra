const std = @import("shim_div.zig");
fn recover(s: []const u8) i32 {
const n: i32 = std.fmt.parseInt(i32, s, 10) catch 0;
return n;
}
pub fn main() void {
    const o = @import("std");
    o.debug.print("{d}\n", .{recover("21")});
}
