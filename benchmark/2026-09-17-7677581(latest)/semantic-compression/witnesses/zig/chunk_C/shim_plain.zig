pub const fmt = struct {
    pub fn allocPrint(a: anytype, comptime f: []const u8, args: anytype) []const u8 {
        _ = a; _ = f; _ = args;
        return "static";
    }
};
