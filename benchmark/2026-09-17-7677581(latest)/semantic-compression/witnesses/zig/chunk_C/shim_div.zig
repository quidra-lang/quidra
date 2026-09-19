pub const fmt = struct {
    pub fn parseInt(comptime T: type, s: []const u8, base: u8) error{Bad}!i32 {
        _ = T; _ = s; _ = base;
        @panic("diverges");
    }
};
