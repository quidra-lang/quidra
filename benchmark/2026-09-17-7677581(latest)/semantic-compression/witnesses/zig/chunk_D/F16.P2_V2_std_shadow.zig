const std = struct {
    pub fn StringHashMap(comptime V: type) type {
        return struct {
            const Self = @This();
            pub const Entry = struct { value_ptr: *V };
            pub fn init(a: anytype) Self {
                _ = a;
                return .{};
            }
            pub fn put(self: *Self, k: []const u8, v: V) !void {
                _ = self;
                _ = k;
                _ = v;
                marker = 1;
            }
            pub fn iterator(self: *const Self) Iter {
                _ = self;
                return .{};
            }
            pub fn get(self: Self, k: []const u8) ?V {
                _ = self;
                _ = k;
                return 5;
            }
            pub const Iter = struct {
                n: u8 = 0,
                pub fn next(it: *Iter) ?Entry {
                    if (it.n == 0) {
                        it.n = 1;
                        return .{ .value_ptr = &shared };
                    }
                    return null;
                }
            };
        };
    }
    pub const mem = struct { pub const Allocator = u8; };
};

var marker: i32 = 0;
var shared: i32 = 9;

fn probe(allocator: std.mem.Allocator) !i32 {
// BEGIN PROBE F16.P2
var mp = std.StringHashMap(i32).init(allocator);
try mp.put("a", 1);
var total: i32 = 0;
var it = mp.iterator();
while (it.next()) |e| total += e.value_ptr.*;
const miss = mp.get("b") orelse 0;
return total + miss;
// END PROBE F16.P2
}

pub fn main() !void {
    const r = try probe(0);
    @import("std").debug.print("result={d} marker={d}\n", .{ r, marker });
}
