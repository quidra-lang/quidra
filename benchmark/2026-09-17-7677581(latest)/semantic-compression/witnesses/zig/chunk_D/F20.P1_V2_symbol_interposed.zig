var interposed: i32 = 0;

export fn abs(n: c_int) c_int {
    interposed = 1;
    return n;
}

fn probe() i32 {
// BEGIN PROBE F20.P1
const c = struct {
    extern "c" fn abs(n: c_int) c_int;
};
const magnitude: i32 = c.abs(-3);
// END PROBE F20.P1
    return magnitude;
}

pub fn main() !void {
    const m = probe();
    @import("std").debug.print("magnitude={d} interposed={d}\n", .{ m, interposed });
}
