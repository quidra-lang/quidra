#!/usr/bin/env python3
"""Frozen generator for ADV-21 (extreme parser nesting), Java.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
  DEPTH copies of '(' , the digit '1', DEPTH copies of ')'.
  No newline inside the parenthesis runs.
Binding type: fixed_width_i64 -> Java `long` (type_binding_table.bindings).
"""
import sys

depth = int(sys.argv[1])
out = sys.argv[2]

prologue = (
    "public class Main {\n"
    "    public static void main(String[] args) {\n"
    "        System.out.println(\"ADV-START\");\n"
    "        System.out.flush();\n"
    "        long v = "
)
epilogue = (
    ";\n"
    "        System.out.println(\"OBS=V:\" + v);\n"
    "        System.out.flush();\n"
    "        System.out.println(\"ADV-END\");\n"
    "        System.out.flush();\n"
    "    }\n"
    "}\n"
)
with open(out, "wb") as fh:
    fh.write(prologue.encode())
    fh.write(b"(" * depth)
    fh.write(b"1")
    fh.write(b")" * depth)
    fh.write(epilogue.encode())
print("depth=%d bytes=%d" % (depth, len(prologue) + 2 * depth + 1 + len(epilogue)))
