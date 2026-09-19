var alloc_calls: usize = 0;

fn cAlloc(ctx: *anyopaque, len: usize, alignment: @import("std").mem.Alignment, ra: usize) ?[*]u8 {
    _ = ctx;
    alloc_calls += 1;
    return @import("std").heap.page_allocator.vtable.alloc(undefined, len, alignment, ra);
}
fn cResize(ctx: *anyopaque, buf: []u8, alignment: @import("std").mem.Alignment, new_len: usize, ra: usize) bool {
    _ = ctx;
    return @import("std").heap.page_allocator.vtable.resize(undefined, buf, alignment, new_len, ra);
}
fn cRemap(ctx: *anyopaque, buf: []u8, alignment: @import("std").mem.Alignment, new_len: usize, ra: usize) ?[*]u8 {
    _ = ctx;
    return @import("std").heap.page_allocator.vtable.remap(undefined, buf, alignment, new_len, ra);
}
fn cFree(ctx: *anyopaque, buf: []u8, alignment: @import("std").mem.Alignment, ra: usize) void {
    _ = ctx;
    @import("std").heap.page_allocator.vtable.free(undefined, buf, alignment, ra);
}
const counting_vtable: @import("std").mem.Allocator.VTable = .{ .alloc = cAlloc, .resize = cResize, .remap = cRemap, .free = cFree };
const counting: @import("std").mem.Allocator = .{ .ptr = undefined, .vtable = &counting_vtable };

fn probe(allocator: @import("std").mem.Allocator) i32 {
// BEGIN PROBE F19.P1
const std = @import("std");
const S = struct {
    fn first() i32 {
        return 20;
    }
    fn second() i32 {
        return 22;
    }
};
var threaded: std.Io.Threaded = .init(allocator, .{});
defer threaded.deinit();
const io = threaded.io();
var f1 = io.async(S.first, .{});
var f2 = io.async(S.second, .{});
const sum = f1.await(io) + f2.await(io);
// END PROBE F19.P1
    return sum;
}

pub fn main() !void {
    const s = @import("std");
    var threaded: s.Io.Threaded = .init_single_threaded;
    const io = threaded.io();
    var buf: [64]u8 = undefined;
    var out = s.Io.File.stdout().writer(io, &buf);
    const r1 = probe(counting);
    try out.interface.print("V1b(pool): sum={d} alloc_calls={d}\n", .{ r1, alloc_calls });
    alloc_calls = 0;
    const r2 = probe(s.mem.Allocator.failing);
    try out.interface.print("V1a(inline): sum={d} alloc_calls={d}\n", .{ r2, alloc_calls });
    try out.interface.flush();
}
