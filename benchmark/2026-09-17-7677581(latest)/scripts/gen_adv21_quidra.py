#!/usr/bin/env python3
"""Frozen generator for ADV-21 (methodology 08 frozen_generators.parser_nesting), Quidra.

DEPTH copies of '(' + '1' + DEPTH copies of ')' as the right-hand side of an
assignment to a binding of the fixed_width_i64 type, between the skeleton
prologue and epilogue.  No newline is inserted inside the parenthesis runs.
"""
import sys, os

PROLOGUE = 'print("ADV-START")\nint64 value = '
EPILOGUE = '\nprint("OBS=V:{value}")\nprint("ADV-END")\n'

def emit(path, depth):
    with open(path, "w") as f:
        f.write(PROLOGUE)
        f.write("(" * depth)
        f.write("1")
        f.write(")" * depth)
        f.write(EPILOGUE)

if __name__ == "__main__":
    out = sys.argv[1]
    emit(os.path.join(out, "ADV-21_single.qui"), 100000)
    for d in (1000, 10000):
        emit(os.path.join(out, "ADV-21_secondary_%d.qui" % d), d)
    print("generated")
