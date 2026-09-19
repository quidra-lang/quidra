const Record = extern struct { count: i32, weight: f64 };

extern "c" fn qsort(base: ?*anyopaque, n: usize, size: usize,
                    cmp: *const fn (?*const anyopaque, ?*const anyopaque) callconv(.c) c_int) void;
extern "c" fn printf(fmt: [*:0]const u8, ...) c_int;

fn cmpDouble(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x: f64 = @as(*const f64, @ptrCast(@alignCast(a.?))).*;
    const y: f64 = @as(*const f64, @ptrCast(@alignCast(b.?))).*;
    if (x > y) return 1;
    if (x < y) return -1;
    return 0;
}
fn cmpRecord(a: ?*const anyopaque, b: ?*const anyopaque) callconv(.c) c_int {
    const x = @as(*const Record, @ptrCast(@alignCast(a.?))).count;
    const y = @as(*const Record, @ptrCast(@alignCast(b.?))).count;
    if (x > y) return 1;
    if (x < y) return -1;
    return 0;
}

pub fn main() void {
    _ = printf("sizeof=%zu offset=%zu\n", @as(usize, @sizeOf(Record)), @as(usize, @offsetOf(Record, "weight")));

    var xs = [_]f64{ 3.5, 1.25, 4.75, 1.5, 2.25 };
    qsort(@ptrCast(&xs), xs.len, @sizeOf(f64), &cmpDouble);
    for (xs, 0..) |v, i| {
        _ = printf(if (i == 0) "%.2f" else " %.2f", v);
    }
    _ = printf("\n");

    var rs = [_]Record{ .{ .count = 3, .weight = 1.5 }, .{ .count = 1, .weight = 4.0 }, .{ .count = 2, .weight = 2.5 } };
    qsort(@ptrCast(&rs), rs.len, @sizeOf(Record), &cmpRecord);
    for (rs, 0..) |r, i| {
        _ = printf(if (i == 0) "%d:%.2f" else " %d:%.2f", @as(c_int, r.count), r.weight);
    }
    _ = printf("\n");
}
