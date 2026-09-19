#!/usr/bin/env python3
"""Frozen generator for ADV-21 (Zig): DEPTH nested parentheses around the literal 1.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
"""
import os
D = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(D, "..", "standard", "src", "adversarial", "zig")
PRO = ('const std = @import("std");\n'
       '\n'
       'pub fn main() !void {\n'
       '    var t = std.Io.Threaded.init_single_threaded;\n'
       '    const io = t.io();\n'
       '    const out = std.Io.File.stdout();\n'
       '    try out.writeStreamingAll(io, "ADV-START\\n");\n'
       '\n'
       '    const v: i64 = ')
EPI = (';\n'
       '\n'
       '    var ob: [64]u8 = undefined;\n'
       '    try out.writeStreamingAll(io, try std.fmt.bufPrint(&ob, "OBS=V:{d}\\n", .{v}));\n'
       '    try out.writeStreamingAll(io, "ADV-END\\n");\n'
       '}\n')
for depth, name in ((100000, "ADV-21.zig"),
                    (1000, "ADV-21_depth1000.zig"),
                    (10000, "ADV-21_depth10000.zig")):
    with open(os.path.join(OUT, name), "wb") as f:
        f.write(PRO.encode() + b"(" * depth + b"1" + b")" * depth + EPI.encode())
    print(name, depth)
