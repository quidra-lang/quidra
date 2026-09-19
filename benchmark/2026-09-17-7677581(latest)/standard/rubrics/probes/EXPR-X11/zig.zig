const std = @import("std");
fn fact(comptime n: u32) u32 { return if (n <= 1) 1 else n * fact(n - 1); }
const V: u32 = fact(5);
comptime { if (V != 120) @compileError("not evaluated before run time"); }
pub fn main() void { var arr: [V]u8 = undefined; _ = &arr; std.debug.print("X11 {d}\n", .{arr.len}); }
