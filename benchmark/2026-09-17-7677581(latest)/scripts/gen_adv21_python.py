#!/usr/bin/env python3
"""Frozen generator for ADV-21 (parser nesting), Python target.

Implements methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting:
DEPTH copies of '(' , then '1', then DEPTH copies of ')'. No newline inside runs.
"""
import sys

PROLOGUE = 'import ctypes\n\nprint("ADV-START", flush=True)\nv: ctypes.c_int64 = '
EPILOGUE = '\nprint("OBS=V:" + str(v), flush=True)\nprint("ADV-END", flush=True)\n'


def emit(path, depth):
    with open(path, "w", encoding="ascii") as fh:
        fh.write(PROLOGUE)
        fh.write("(" * depth)
        fh.write("1")
        fh.write(")" * depth)
        fh.write(EPILOGUE)


if __name__ == "__main__":
    emit(sys.argv[1], int(sys.argv[2]))
