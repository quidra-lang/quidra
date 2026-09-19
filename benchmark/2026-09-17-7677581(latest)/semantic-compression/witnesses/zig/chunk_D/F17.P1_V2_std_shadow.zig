const std = struct {
    pub const Io = struct {
        pub const Dir = struct {
            pub fn cwd() Dir {
                return .{};
            }
            pub fn openFile(d: Dir, i: anytype, p: []const u8, o: anytype) !File {
                _ = d;
                _ = i;
                _ = p;
                _ = o;
                opens += 1;
                return .{};
            }
        };
        pub const File = struct {
            pub fn close(f: File, i: anytype) void {
                _ = f;
                _ = i;
                closes += 1;
            }
            pub fn reader(f: File, i: anytype, b: []u8) Rdr {
                _ = f;
                _ = i;
                _ = b;
                return .{};
            }
        };
        pub const Rdr = struct {
            interface: Iface = .{},
            pub const Iface = struct {
                pub fn allocRemaining(s: *Iface, a: anytype, l: anytype) ![]u8 {
                    _ = s;
                    _ = a;
                    _ = l;
                    return @constCast("shadowed-text");
                }
            };
        };
    };
    pub const heap = struct { pub const page_allocator: u8 = 0; };
};


var io: u8 = 0;
var opens: i32 = 0;
var closes: i32 = 0;

// BEGIN PROBE F17.P1
fn readAll() !usize {
    const file = try std.Io.Dir.cwd().openFile(io, "data.txt", .{});
    defer file.close(io);
    var reader = file.reader(io, &.{});
    const text = try reader.interface.allocRemaining(std.heap.page_allocator, .unlimited);
    return text.len;
}
// END PROBE F17.P1

pub fn main() !void {
    const n = try readAll();
    @import("std").debug.print("len={d} opens={d} closes={d}\n", .{ n, opens, closes });
}
