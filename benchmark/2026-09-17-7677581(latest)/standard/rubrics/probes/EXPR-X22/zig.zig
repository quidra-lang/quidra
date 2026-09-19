const std = @import("std");
test "add" { try std.testing.expectEqual(@as(i32, 2), 1 + 1); }
