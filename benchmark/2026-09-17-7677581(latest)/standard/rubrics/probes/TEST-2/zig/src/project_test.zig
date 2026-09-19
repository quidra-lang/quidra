const std = @import("std");
const mod_a = @import("mod_a.zig");
const mod_b = @import("mod_b.zig");

test "scale" {
    try std.testing.expect(mod_a.scale(2) == 6);
}

test "scale_and_offset" {
    try std.testing.expect(mod_b.scaleAndOffset(2) == 7);
}
