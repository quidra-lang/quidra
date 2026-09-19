const mod_a = @import("mod_a.zig");

pub fn scaleAndOffset(x: i32) i32 {
    return mod_a.scale(x) + 1;
}
