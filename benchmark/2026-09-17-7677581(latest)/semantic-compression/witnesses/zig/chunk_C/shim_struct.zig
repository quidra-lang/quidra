pub const Rec = struct { n: usize };
pub const fmt = struct {
    pub fn allocPrint(a: anytype, comptime f: []const u8, args: anytype) Rec {
        _ = a; _ = f; _ = args;
        return .{ .n = 7 };
    }
};
