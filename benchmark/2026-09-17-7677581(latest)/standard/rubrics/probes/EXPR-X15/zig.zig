const std = @import("std");
pub fn main() !void {
    const t = "h\u{e9}llo";
    var it = (try std.unicode.Utf8View.init(t)).iterator();
    var n: usize = 0;
    while (it.nextCodepoint()) |cp| { _ = cp; n += 1; }
    std.debug.print("X15 {d}\n", .{n});
}
