const std = struct {
    pub fn ArrayList(comptime T: type) type {
        return struct {
            items: []const T,
            pub const empty: @This() = .{ .items = &.{} };
            pub fn appendSlice(self: *@This(), a: anytype, s: []const T) !void {
                _ = a;
                _ = s;
                marker = 1;
                self.items = &.{ 7, 7 };
            }
        };
    }
    pub const mem = struct { pub const Allocator = u8; };
};

var marker: i32 = 0;

fn probe(allocator: std.mem.Allocator) !i32 {
// BEGIN PROBE F16.P1
var xs: std.ArrayList(i32) = .empty;
try xs.appendSlice(allocator, &.{ 1, 2, 3 });
var total: i32 = 0;
for (xs.items) |x| total += x;
return total;
// END PROBE F16.P1
}

pub fn main() !void {
    const r = try probe(0);
    @import("std").debug.print("total={d} marker={d}\n", .{ r, marker });
}
