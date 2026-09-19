pub const fmt = struct {
    pub fn parseInt(comptime T: type, s: []const u8, base: u8) error{Bad}!i8 {
        _ = T; _ = base;
        if (s.len == 0) return error.Bad;
        return @intCast(s[0] - '0');
    }
};
