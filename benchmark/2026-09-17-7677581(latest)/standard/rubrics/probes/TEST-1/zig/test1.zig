// TEST-1 probe, Zig. Exactly 3 tests using the language's built-in `test` blocks
// and std.testing: two assert a true condition, one asserts a false condition.
const std = @import("std");

fn add(a: i32, b: i32) i32 {
    return a + b;
}

test "pass_one" {
    try std.testing.expect(add(1, 1) == 2);
}

test "pass_two" {
    try std.testing.expect(add(2, 3) == 5);
}

test "fail_one" {
    try std.testing.expect(add(1, 1) == 3);
}
