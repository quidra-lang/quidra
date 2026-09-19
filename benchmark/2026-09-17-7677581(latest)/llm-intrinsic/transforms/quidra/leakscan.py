#!/usr/bin/env python3
"""
Identity-leak scan of the Reference Pack (methodology 10 section 2.6).

Rejects the pack if it contains, as a WHOLE WORD, case-insensitively:
  - any real reserved word of the language the pack anonymizes;
  - the language's own name, its file extension, or its tool names.

Usage: python3 leakscan.py reference_pack.md
"""

import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))

LEAK_TERMS = [
    "quidra", "qui", "quidralang", "quistate",
    # the other nine languages of the fixed set, so the pack names no language at all
    "python", "cpp", "c++", "rust", "rustc", "golang", "java", "javac", "kotlin",
    "kotlinc", "swift", "swiftc", "typescript", "tsc", "node", "zig", "clang",
    "jvm", "npm", "cargo", "pip",
]


def main(path):
    text = open(path, encoding="utf-8").read()
    doc = json.load(open(os.path.join(HERE, "mapping.json"), encoding="ascii"))
    reserved = sorted(doc["mapping"].keys())
    extra_reserved = ["none", "int8", "int16", "int32", "int64", "uint8", "uint16",
                      "uint32", "uint64", "float32", "float64", "extern", "cli"]
    hits = {}
    for term in reserved + extra_reserved + LEAK_TERMS:
        found = [m.start() for m in re.finditer(r"\b%s\b" % re.escape(term), text, re.I)]
        if found:
            lines = sorted({text.count("\n", 0, p) + 1 for p in found})
            hits[term] = lines
    for term, lines in sorted(hits.items()):
        print("LEAK %-10s lines %s" % (term, lines))
    lines = text.count("\n") + (0 if text.endswith("\n") else 1)
    print("lines=%d chars=%d" % (lines, len(text)))
    print("LEAK SCAN:", "PASS (zero hits)" if not hits else "FAIL (%d terms)" % len(hits))
    return 0 if not hits else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1]))
