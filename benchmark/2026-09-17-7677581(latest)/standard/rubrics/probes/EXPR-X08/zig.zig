const std = @import("std");
const E = error{Boom};
fn inner() E!i32 { return E.Boom; }
fn outer() E!i32 { const v = try inner(); return v; }
pub fn main() void { _ = outer() catch { std.debug.print("X08 caught boom\n", .{}); return; }; }
