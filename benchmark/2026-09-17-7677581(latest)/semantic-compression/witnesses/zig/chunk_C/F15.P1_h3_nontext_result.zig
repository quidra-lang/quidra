const std = @import("shim_struct.zig");
const RT = if (@hasDecl(std, "Rec")) std.Rec else []const u8;
fn combine(allocator: u8) RT {
const head = struct {
    fn head(comptime T: type, xs: []const T) T {
        return xs[0];
    }
}.head;
const a = head(i32, &.{ 4, 5, 6 });
const b = head([]const u8, &.{ "p", "q" });
return std.fmt.allocPrint(allocator, "{d}{s}", .{ a, b });
}
pub fn main() void {
    const o = @import("std");
    o.debug.print("{any}\n", .{combine(0)});
}
