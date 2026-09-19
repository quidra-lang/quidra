const std = @import("std");

pub fn main () ! void {
    var tio: std.Io.Threaded = .init_single_threaded;
    const io = tio.io();
    var wbuf: [512] u8 = undefined ;
    var out = std.Io.File.stdout().writer(io, &wbuf);

    var state: u64 = 7;
    var sum: u64 = 0;
    var largest: u64 = 0;
    var evens: u64 = 0;
    var first: [5] u64 = undefined ;

    var i: u64 = 0;
    while (i < 50) : (i = i + 1) {
        state = (state * 48271) % 2147483647;
        const term: u64 = state % 1000;
        sum = sum + term;
        if (term > largest) {
            largest = term;
        }
        if (term % 2 == 0) {
            evens = evens + 1;
        }
        if (i < 5) {
            first[i] = term;
        }
    }

    try out.interface.print("SUM {d}\n", .{sum});
    try out.interface.print("MAX {d}\n", .{largest});
    try out.interface.print("EVENS {d}\n", .{evens});
    try out.interface.print("JOINED {d}-{d}-{d}-{d}-{d}\n", .{ first[0], first[1], first[2], first[3], first[4] });
    try out.interface.flush();
}
