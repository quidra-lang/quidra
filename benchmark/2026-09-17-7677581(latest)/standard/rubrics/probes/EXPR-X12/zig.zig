const std = @import("std");
const Rec = struct { a: i32, b: i32 };
pub fn main() void { std.debug.print("X12 meta {d}\n", .{@typeInfo(Rec).@"struct".fields.len}); }
