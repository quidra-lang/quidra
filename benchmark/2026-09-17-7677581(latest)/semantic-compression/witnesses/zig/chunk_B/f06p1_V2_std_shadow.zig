const std = struct {
    pub fn ArrayList(comptime T: type) type {
        return struct {
            items: []const T,
            pub const empty: @This() = .{ .items = &.{} };
            pub fn appendSlice(self: *@This(), a: anytype, s: []const T) !void {
                _ = a;
                self.items = s;
            }
        };
    }
    pub const mem = struct { pub const Allocator = u8; };
};
fn probe(allocator: u8) !i32 {
var xs: std.ArrayList(i32) = .empty;
try xs.appendSlice(allocator, &.{ 1, 2, 3 });
const y = mid(xs);
return y;
}

fn mid(v: std.ArrayList(i32)) i32 {
    return v.items[1] + 10;
}
pub fn main() !void {
    const r = try probe(0);

    @import("std").debug.print("{d}\n", .{r});
}
