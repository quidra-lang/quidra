// ffi4_supplied.zig here is a symlink to ../../../build/ffi4_supplied.zig, the file the
// frozen recipe's `zig translate-c` command writes. Zig refuses @import of a path outside
// the root module directory, so the generated file is surfaced inside it by symlink.
// Declarations are consumed mechanically from probes/FFI-4/c/supplied.h by
// `zig translate-c`; nothing in supplied.h is hand-transcribed here.
const c = @import("ffi4_supplied.zig");
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;

pub fn main() void {
    var r: c.SuppliedRecord = .{ .count = 3, .weight = 1.5 };
    _ = printf("supplied_add=%d\n", c.supplied_add(20, 22));
    _ = printf("supplied_weighted=%.2f\n", c.supplied_weighted(&r));
    _ = printf("SUPPLIED_SCALE=%d\n", @as(c_int, c.SUPPLIED_SCALE));
}
