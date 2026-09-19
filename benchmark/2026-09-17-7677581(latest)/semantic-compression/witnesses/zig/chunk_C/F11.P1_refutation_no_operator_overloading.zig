const std = @import("std");
const Seq = struct {
    buf: [3]i32,
    pub fn index(self: Seq, k: usize) i32 { return self.buf[k]; }
};
const Box = struct { items: Seq };
fn read_at(xs: Box, i: i32) i32 {
const e = xs.items[@intCast(i)];
return e;
}
pub fn main() void { std.debug.print("{d}\n", .{read_at(.{ .items = .{ .buf = .{1,2,3} } }, 2)}); }
