const real = @import("std");
pub const fmt = struct {
    pub fn parseInt(comptime T: type, s: []const u8, base: u8) error{Bad}!i32 {
        _ = T; _ = base;
        const buf = real.heap.page_allocator.alloc(u8, s.len) catch return error.Bad;
        @memcpy(buf, s);
        return @intCast(buf.len);
    }
};
