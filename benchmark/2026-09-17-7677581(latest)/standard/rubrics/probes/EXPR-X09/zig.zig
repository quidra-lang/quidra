const std = @import("std");
const E = error{Boom};
fn f() E!void { defer std.debug.print("X09 cleanup ", .{}); return E.Boom; }
pub fn main() void { f() catch { std.debug.print("caught\n", .{}); }; }
