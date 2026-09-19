#!/usr/bin/env python3
"""Frozen generator for ADV-21 (extreme parser nesting), Kotlin.

methodology/08_adversarial_cases.json -> frozen_generators.parser_nesting
  DEPTH copies of '(' , the digit '1', DEPTH copies of ')'.
  No newline inside the parenthesis runs.
Binding type: fixed_width_i64 -> Kotlin `Long` (type_binding_table.bindings).
"""
import sys

depth = int(sys.argv[1])
out = sys.argv[2]

prologue = (
    "fun main() {\n"
    "    println(\"ADV-START\")\n"
    "    System.out.flush()\n"
    "    val v: Long = "
)
epilogue = (
    "\n"
    "    println(\"OBS=V:\" + v)\n"
    "    System.out.flush()\n"
    "    println(\"ADV-END\")\n"
    "    System.out.flush()\n"
    "}\n"
)
with open(out, "wb") as fh:
    fh.write(prologue.encode())
    fh.write(b"(" * depth)
    fh.write(b"1")
    fh.write(b")" * depth)
    fh.write(epilogue.encode())
print("depth=%d bytes=%d" % (depth, len(prologue) + 2 * depth + 1 + len(epilogue)))
